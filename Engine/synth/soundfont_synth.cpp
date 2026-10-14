/**
 * @file    soundfont_synth.cpp
 * @brief   SoundFont synth engine implementation using TinySoundFont & SF2Cache.
 *
 * Threading model (same as the other engines, see synth_engine.cpp)
 * -----------------------------------------------------------------
 * TinySoundFont keeps all of its state (voices, channels) in one structure that is NOT thread
 * safe: tsf_note_on() publishes a voice (playingPreset) before its envelopes, pan and pitch are
 * filled in. MIDI events arrive in the main context while tsf_render_*() runs in the audio
 * context, so the events must never call TinySoundFont directly. They are put into a lock-free
 * SPSC FIFO (producer: main, consumer: audio) and applied at the start of the next render.
 *
 * File I/O stays in the main context: before the NoteOn event is queued, the first blocks of
 * the sample data the new voices will read are loaded into the SF2 cache, so the attack of the
 * note is never replaced by silence.
 */
#include "soundfont_synth.h"
#include "sf2_cache.h"
#include "tsf.h"

#if !SF2_HOST_MODE
#include "stm32h7xx.h"   /* DWT cycle counter for the diagnostics */
#ifdef PURRMIDI_SF2_XIP
#include "sf2_loader.h"
#include "xip_diag.h"
#endif
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>

/* Number of blocks (SF2_BLOCK_SAMPLES samples each) loaded synchronously per voice at NoteOn.
 * Only the SD streaming cache needs this; with QSPI XIP the data is always in place. */
#ifndef SF2_PRELOAD_BLOCKS_PER_VOICE
#define SF2_PRELOAD_BLOCKS_PER_VOICE 2U
#endif

/* 1 = collect per-block statistics (render time, voices, clipping) and print them from the main loop. */
#ifndef SF2_DIAG_STATS
#define SF2_DIAG_STATS 1
#endif

#define SF_EVENT_FIFO_SIZE 256U   /* power of two */
#define SF_MAX_NOTE_REGIONS 8

namespace {

tsf  *g_tsf = nullptr;
bool  g_loaded = false;
bool  g_init_attempted = false;   /* SoundFontSynth_InitSF2() has been run at least once */

enum EventType : uint8_t { EV_NOTE_ON = 1, EV_NOTE_OFF, EV_CC, EV_PROGRAM };

struct Event {
    uint8_t type;
    uint8_t a;
    uint8_t b;
};

/* Lock-free SPSC FIFO: producer - main, consumer - audio. */
Event                 g_fifo[SF_EVENT_FIFO_SIZE];
std::atomic<uint32_t> g_fifo_head{0};
std::atomic<uint32_t> g_fifo_tail{0};
std::atomic<uint32_t> g_dropped{0};

bool fifo_push(const Event &ev)
{
    uint32_t head = g_fifo_head.load(std::memory_order_relaxed);
    uint32_t tail = g_fifo_tail.load(std::memory_order_acquire);

    if (head - tail >= SF_EVENT_FIFO_SIZE) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_fifo[head & (SF_EVENT_FIFO_SIZE - 1U)] = ev;
    g_fifo_head.store(head + 1U, std::memory_order_release);
    return true;
}

bool fifo_pop(Event &ev)
{
    uint32_t tail = g_fifo_tail.load(std::memory_order_relaxed);
    uint32_t head = g_fifo_head.load(std::memory_order_acquire);

    if (head == tail) {
        return false;
    }
    ev = g_fifo[tail & (SF_EVENT_FIFO_SIZE - 1U)];
    g_fifo_tail.store(tail + 1U, std::memory_order_release);
    return true;
}

void fifo_reset()
{
    g_fifo_head.store(0, std::memory_order_relaxed);
    g_fifo_tail.store(0, std::memory_order_relaxed);
}

float velocity_to_float(uint8_t velocity)
{
    return (float)velocity / 127.0f;
}

/* ---- statistics (written in the audio context, read and cleared in the main loop) ---- */
struct DiagStats {
    volatile uint32_t blocks;          /* audio blocks rendered in this window                         */
    volatile uint32_t cycles_sum;      /* CPU cycles spent in SoundFontSynth_FillStereoBuffer          */
    volatile uint32_t cycles_max;
    volatile uint32_t over_budget;     /* blocks that took longer than one block period                */
    volatile uint32_t voices_max;      /* active TinySoundFont voices (max over the window)            */
    volatile uint32_t voices_last;
    volatile uint32_t peak_permille;   /* largest |sample| of the MIX before limiting, 1000 = full scale */
    volatile uint32_t over_fs_blocks;  /* blocks whose mix exceeded full scale (would clip)            */
    volatile uint32_t over_fs_samples; /* samples of the mix above full scale                          */
    volatile uint32_t notes_on;        /* NoteOn events applied                                        */
    volatile uint32_t regions_wanted;  /* voices the bank asks for (regions matching key/velocity)     */
    volatile uint32_t voices_started;  /* growth of the active voice count after NoteOn                */
    volatile uint32_t voice_shortfall; /* wanted - started: voices stolen from releasing notes or lost */
    volatile uint32_t regions_max;     /* most regions of one note seen (session)                      */
};
DiagStats g_diag;
uint32_t  g_diag_fifo_dropped_seen = 0;

#if !SF2_HOST_MODE
inline uint32_t diag_cycles() { return DWT->CYCCNT; }
inline uint32_t diag_cycles_per_us() { const uint32_t v = SystemCoreClock / 1000000U; return v ? v : 1U; }
#else
inline uint32_t diag_cycles() { return 0U; }
inline uint32_t diag_cycles_per_us() { return 1U; }
#endif

/* Smooth limiter: identity below the knee, above it a tanh-shaped approach to the ceiling (the value and the
 * slope are continuous at the knee). tanh(t) ~ t*(27+t^2)/(27+9*t^2) is exact enough for |t| < 3 and reaches
 * 1.0 with zero slope at t = 3, so the output never exceeds SF2_LIMITER_CEILING. */
inline float soft_limit(float x)
{
    const float knee = SF2_LIMITER_KNEE;
    const float range = SF2_LIMITER_CEILING - knee;
    const float a = (x < 0.0f) ? -x : x;
    if (a <= knee) return x;
    const float t = (a - knee) / range;
    float y;
    if (t >= 3.0f) {
        y = SF2_LIMITER_CEILING;
    } else {
        const float t2 = t * t;
        y = knee + range * (t * (27.0f + t2) / (27.0f + 9.0f * t2));
    }
    return (x < 0.0f) ? -y : y;
}

constexpr uint32_t kRenderChunkFrames = 128U;

/* Render num_frames of stereo audio as float, measure the mix, limit it, convert to int16. */
void render_block(int16_t *out, uint32_t num_frames)
{
    float mix[kRenderChunkFrames * 2U];

    while (num_frames) {
        const uint32_t n = (num_frames > kRenderChunkFrames) ? kRenderChunkFrames : num_frames;
        tsf_render_float(g_tsf, mix, (int)n, 0);

        float peak = 0.0f;
        uint32_t over = 0;
        for (uint32_t i = 0, cnt = n * 2U; i < cnt; ++i) {
            float x = mix[i];
            const float a = (x < 0.0f) ? -x : x;
            if (a > peak) peak = a;
            if (a > 1.0f) ++over;
#if SF2_SOFT_LIMITER
            x = soft_limit(x);
#else
            x = (x < -1.0f) ? -1.0f : (x > 1.0f ? 1.0f : x);
#endif
            out[i] = (int16_t)(x * 32767.0f);
        }
#if SF2_DIAG_STATS
        const uint32_t pm = (uint32_t)(peak * 1000.0f);
        if (pm > g_diag.peak_permille) g_diag.peak_permille = pm;
        if (over) {
            g_diag.over_fs_blocks++;
            g_diag.over_fs_samples += over;
        }
#endif
        out += n * 2U;
        num_frames -= n;
    }
}

/* Audio context: apply one queued MIDI event to TinySoundFont. */
void apply_event(const Event &ev)
{
    switch (ev.type) {
        case EV_NOTE_ON:
#if SF2_DIAG_STATS
        {
            unsigned int starts[SF_MAX_NOTE_REGIONS];
            const int wanted = tsf_channel_get_note_start_samples(g_tsf, 0, (int)ev.a, velocity_to_float(ev.b),
                                                                  starts, SF_MAX_NOTE_REGIONS);
            const int before = tsf_active_voice_count(g_tsf);
            tsf_channel_note_on(g_tsf, 0, (int)ev.a, velocity_to_float(ev.b));
            const int started = tsf_active_voice_count(g_tsf) - before;
            g_diag.notes_on++;
            g_diag.regions_wanted += (uint32_t)wanted;
            g_diag.voices_started += (uint32_t)(started > 0 ? started : 0);
            if (wanted > started) {
                g_diag.voice_shortfall += (uint32_t)(wanted - started);
            }
            if ((uint32_t)wanted > g_diag.regions_max) {
                g_diag.regions_max = (uint32_t)wanted;
            }
        }
#else
            tsf_channel_note_on(g_tsf, 0, (int)ev.a, velocity_to_float(ev.b));
#endif
            break;
        case EV_NOTE_OFF:
            tsf_channel_note_off(g_tsf, 0, (int)ev.a);
            break;
        case EV_CC:
            tsf_channel_midi_control(g_tsf, 0, (int)ev.a, (int)ev.b);
            break;
        case EV_PROGRAM:
            tsf_channel_set_presetnumber(g_tsf, 0, (int)ev.a, 0);
            break;
        default:
            break;
    }
}

#ifndef PURRMIDI_SF2_XIP
/* Main context: make the first blocks of every voice of this note resident before it starts. */
void preload_note(uint8_t note, uint8_t velocity)
{
    unsigned int starts[SF_MAX_NOTE_REGIONS];
    int n = tsf_channel_get_note_start_samples(g_tsf, 0, (int)note, velocity_to_float(velocity),
                                               starts, SF_MAX_NOTE_REGIONS);
    if (n > SF_MAX_NOTE_REGIONS) {
        n = SF_MAX_NOTE_REGIONS;
    }
    for (int i = 0; i < n; ++i) {
        uint32_t first_block = starts[i] / SF2_BLOCK_SAMPLES;
        for (uint32_t k = 0; k < SF2_PRELOAD_BLOCKS_PER_VOICE; ++k) {
            SF2Cache_PreloadBlock(first_block + k);
        }
    }
}
#endif /* !PURRMIDI_SF2_XIP */

} // namespace

extern "C" {

bool SoundFontSynth_InitSF2(void)
{
    if (g_tsf) {
        tsf_close(g_tsf);
        g_tsf = nullptr;
    }
    g_loaded = false;
    g_init_attempted = true;
    fifo_reset();

    std::fprintf(stderr, "[SF2] Opening 0:/SNDFNT.SF2 (SD mount + FatFs)...\n");
    if (!SF2Cache_OpenFile("0:/SNDFNT.SF2")) {
        std::fprintf(stderr, "[SF2] Failed to open SoundFont file; see the attempted host paths above.\n");
        return false;
    }

    struct tsf_stream stream;
    SF2Cache_InitStream(&stream);

    std::fprintf(stderr, "[SF2] File opened, parsing presets (tsf_load)...\n");
    g_tsf = tsf_load(&stream);
    if (!g_tsf) {
        std::fprintf(stderr, "[SF2] tsf_load() failed. The file may be invalid, truncated, unsupported, or unreadable.\n");
        SF2Cache_CloseFile();
        return false;
    }

    std::fprintf(stderr, "[SF2] SoundFont metadata loaded successfully.\n");

    tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, 48000, SF2_GLOBAL_GAIN_DB);
    tsf_set_max_voices(g_tsf, 32);
    SF2Cache_SetSmplFileOffset(tsf_get_smpl_file_offset(g_tsf), tsf_get_smpl_sample_count(g_tsf));

    /* MIDI channel 0 must exist before the first note: tsf_channel_note_on() ignores notes
     * for channels that have not been created. */
    tsf_channel_set_presetnumber(g_tsf, 0, 0, 0);

    g_loaded = true;
    return true;
}

void SoundFontSynth_Init(void)
{
    /* Try to load the SoundFont only ONCE. Retrying on every engine selection would block the
     * main loop (SD init / file parsing) each time the user cycles through the instruments
     * when the card or the file is missing. */
    if (!g_loaded && !g_tsf && !g_init_attempted) {
        SoundFontSynth_InitSF2();
    }

    if (g_tsf) {
        tsf_reset(g_tsf);
        tsf_channel_set_presetnumber(g_tsf, 0, 0, 0);
    }
    SF2Cache_Reset();
    fifo_reset();
}

void SoundFontSynth_NoteOn(uint8_t note, uint8_t velocity)
{
    if (!g_loaded || !g_tsf) return;

    if (velocity == 0) {
        SoundFontSynth_NoteOff(note);
        return;
    }
    if (note > 127) return;
    if (velocity > 127) velocity = 127;

#ifndef PURRMIDI_SF2_XIP
    preload_note(note, velocity);
#endif

    Event ev = { EV_NOTE_ON, note, velocity };
    fifo_push(ev);
}

void SoundFontSynth_NoteOff(uint8_t note)
{
    if (!g_loaded || !g_tsf) return;
    if (note > 127) return;

    Event ev = { EV_NOTE_OFF, note, 0 };
    fifo_push(ev);
}

void SoundFontSynth_ControlChange(uint8_t control, uint8_t value)
{
    if (!g_loaded || !g_tsf) return;
    if (control > 127) return;

    Event ev = { EV_CC, control, (uint8_t)(value > 127 ? 127 : value) };
    fifo_push(ev);
}

void SoundFontSynth_ProgramChange(uint8_t program)
{
    if (!g_loaded || !g_tsf) return;

    Event ev = { EV_PROGRAM, program, 0 };
    fifo_push(ev);
}

void SoundFontSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames)
{
    if (!g_loaded || !g_tsf) {
        std::memset(buffer, 0, (size_t)num_frames * 2U * sizeof(int16_t));
        return;
    }

    Event ev;
    while (fifo_pop(ev)) {
        apply_event(ev);
    }

    SF2Cache_BeginAudioBlock();
#if SF2_DIAG_STATS
    const uint32_t t0 = diag_cycles();
    render_block(buffer, num_frames);
    const uint32_t dt = diag_cycles() - t0;
    const uint32_t voices = (uint32_t)tsf_active_voice_count(g_tsf);

    g_diag.blocks++;
    g_diag.cycles_sum += dt;
    if (dt > g_diag.cycles_max) g_diag.cycles_max = dt;
#if !SF2_HOST_MODE
    if (dt > (SystemCoreClock / 48000U) * num_frames) g_diag.over_budget++;
#endif
    g_diag.voices_last = voices;
    if (voices > g_diag.voices_max) g_diag.voices_max = voices;
#else
    render_block(buffer, num_frames);
#endif
}

void SoundFontSynth_Process(void)
{
    SF2Cache_ProcessRequests();
}

bool SoundFontSynth_IsLoaded(void)
{
    return g_loaded;
}

bool SoundFontSynth_IsAvailable(void)
{
    return g_loaded || !g_init_attempted;
}


/* ------------------------------------------------------------------------------------------------
 * Diagnostics
 *   - SoundFontSynth_ReportStats(): one line per second while notes are playing (see soundfont_synth.h)
 *   - SoundFontSynth_Profile():     boot-time report: XIP configuration, raw XIP read speed, regions per
 *                                   note and the cost of 1..10 note chords rendered by the real engine
 * ------------------------------------------------------------------------------------------------ */

void SoundFontSynth_ResetStats(void)
{
    std::memset((void *)&g_diag, 0, sizeof(g_diag));
}

void SoundFontSynth_ReportStats(uint32_t now_ms)
{
#if SF2_DIAG_STATS
    static uint32_t last_ms = 0;
    static uint32_t last_notes = 0;

    if ((now_ms - last_ms) < 1000U) {
        return;
    }
    last_ms = now_ms;

    /* Quiet when nothing is happening: no notes were started and no voices are sounding. */
    const uint32_t notes = g_diag.notes_on;
    if (g_diag.blocks == 0U || (notes == last_notes && g_diag.voices_max == 0U)) {
        last_notes = notes;
        return;
    }
    last_notes = notes;

    const uint32_t blocks = g_diag.blocks;
    const uint32_t us = diag_cycles_per_us();
    const uint32_t dropped = g_dropped.load(std::memory_order_relaxed);
    const unsigned long avg_us = (unsigned long)((g_diag.cycles_sum / blocks) / us);
    const unsigned long max_us = (unsigned long)(g_diag.cycles_max / us);

    std::fprintf(stderr,
        "[SF2] 1s: render avg=%lu max=%lu us (over budget=%lu of %lu blocks) | voices max=%lu now=%lu"
        " | notes=%lu regions want=%lu started=%lu short=%lu (max/note=%lu)"
        " | mix peak=%lu.%02lu FS, over 1.0 FS: %lu samples in %lu blocks%s | fifo dropped=%lu\n",
        avg_us, max_us, (unsigned long)g_diag.over_budget, (unsigned long)blocks,
        (unsigned long)g_diag.voices_max, (unsigned long)g_diag.voices_last,
        (unsigned long)g_diag.notes_on, (unsigned long)g_diag.regions_wanted, (unsigned long)g_diag.voices_started,
        (unsigned long)g_diag.voice_shortfall, (unsigned long)g_diag.regions_max,
        (unsigned long)(g_diag.peak_permille / 1000U), (unsigned long)((g_diag.peak_permille / 10U) % 100U),
        (unsigned long)g_diag.over_fs_samples, (unsigned long)g_diag.over_fs_blocks,
        SF2_SOFT_LIMITER ? " (soft limiter ON)" : " (HARD CLIP)",
        (unsigned long)(dropped - g_diag_fifo_dropped_seen));
    g_diag_fifo_dropped_seen = dropped;

    /* New window; the session maximum of regions per note is kept. */
    const uint32_t keep_regions_max = g_diag.regions_max;
    SoundFontSynth_ResetStats();
    g_diag.regions_max = keep_regions_max;
#else
    (void)now_ms;
#endif
}

#if !SF2_HOST_MODE

namespace {

/* Regions (= TinySoundFont voices) one note needs, for a few keys and velocities. */
void diag_regions_per_note(void)
{
    static const uint8_t keys[] = { 36, 48, 60, 72, 84, 96 };
    static const uint8_t vels[] = { 40, 100, 127 };
    unsigned int starts[SF_MAX_NOTE_REGIONS];

    std::fprintf(stderr, "[SF2DIAG] preset 0: \"%s\", presets=%d, sample data %lu samples, max voices 32\n",
                 tsf_get_presetname(g_tsf, 0) ? tsf_get_presetname(g_tsf, 0) : "?", tsf_get_presetcount(g_tsf),
                 (unsigned long)tsf_get_smpl_sample_count(g_tsf));
    std::fprintf(stderr, "[SF2DIAG] voices (regions) per note at velocity 40 / 100 / 127:\n");
    for (unsigned k = 0; k < sizeof(keys); ++k) {
        int n[3];
        for (unsigned v = 0; v < 3U; ++v) {
            n[v] = tsf_channel_get_note_start_samples(g_tsf, 0, keys[k], velocity_to_float(vels[v]), starts, SF_MAX_NOTE_REGIONS);
        }
        std::fprintf(stderr, "[SF2DIAG]   key %3u: %d / %d / %d\n", keys[k], n[0], n[1], n[2]);
    }
}

void diag_chord(const char *tag)
{
    static int16_t buf[256];
    static const uint8_t notes[] = { 48, 52, 55, 60, 64, 67, 72, 76, 79, 84 };
    static const int counts[] = { 1, 3, 6, 10 };
    const uint32_t us = diag_cycles_per_us();
    const uint32_t budget_us = 2666U;
    constexpr int BLOCKS = 60;

    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); ++c) {
        const int n = counts[c];
        SoundFontSynth_Init();
        /* tsf_reset() leaves the previous voices in a short quick-release: let them die before measuring. */
        for (int b = 0; b < 8; ++b) { SoundFontSynth_FillStereoBuffer(buf, 128); }
        SoundFontSynth_ResetStats();
        for (int k = 0; k < n; ++k) { SoundFontSynth_NoteOn(notes[k], 100); }

        uint32_t first = 0, sum = 0, mx = 0;
        int voices = 0;
        for (int b = 0; b < BLOCKS; ++b) {
            const uint32_t t0 = diag_cycles();
            SoundFontSynth_FillStereoBuffer(buf, 128);
            const uint32_t d = diag_cycles() - t0;
            if (b == 0) { first = d; voices = tsf_active_voice_count(g_tsf); }
            sum += d;
            if (d > mx) mx = d;
        }
        std::fprintf(stderr,
            "[SF2DIAG] %s chord n=%2d: voices=%2d | first block=%lu us avg=%lu us max=%lu us (%lu%% of %lu us)%s"
            " | mix peak=%lu.%02lu FS (over 1.0 FS: %lu samples) | want=%lu started=%lu short=%lu\n",
            tag, n, voices, (unsigned long)(first / us), (unsigned long)(sum / BLOCKS / us), (unsigned long)(mx / us),
            (unsigned long)(mx / us * 100U / budget_us), (unsigned long)budget_us, (mx / us > budget_us) ? "  <-- OVER BUDGET" : "",
            (unsigned long)(g_diag.peak_permille / 1000U), (unsigned long)((g_diag.peak_permille / 10U) % 100U),
            (unsigned long)g_diag.over_fs_samples, (unsigned long)g_diag.regions_wanted,
            (unsigned long)g_diag.voices_started, (unsigned long)g_diag.voice_shortfall);
        if (n >= 6 && voices > 0) {
            const uint32_t per_voice = sum / BLOCKS / us / (uint32_t)voices;
            std::fprintf(stderr, "[SF2DIAG]   ~%lu us per voice per block -> about %lu voices fit in 80%% of the block (limit 32)\n",
                         (unsigned long)per_voice, (unsigned long)(budget_us * 80U / 100U / (per_voice ? per_voice : 1U)));
        }
    }
}

} // namespace

void SoundFontSynth_Profile(void)
{
    if (!g_loaded || !g_tsf) {
        std::fprintf(stderr, "[SF2DIAG] SoundFont not loaded, skipped\n");
        return;
    }
    std::fprintf(stderr, "[SF2DIAG] ===== SoundFont diagnostics (render budget 2666 us per 128-frame block) =====\n");
#ifdef __OPTIMIZE__
    std::fprintf(stderr, "[SF2DIAG] soundfont_synth.cpp: optimized build\n");
#else
    std::fprintf(stderr, "[SF2DIAG] soundfont_synth.cpp: NOT OPTIMIZED (-O0): render cost below is several times higher than in Release\n");
#endif
    diag_regions_per_note();

#ifdef PURRMIDI_SF2_XIP
    const uint8_t *bank = nullptr;
    uint32_t bank_size = 0;
    XipDiag_PrintConfig();
    if (SF2Loader_GetBank(&bank, &bank_size)) {
        std::fprintf(stderr, "[SF2DIAG] bank in XIP: %p, %lu bytes\n", (const void *)bank, (unsigned long)bank_size);
        XipDiag_Bench(bank, bank_size);
    }
#endif

    std::fprintf(stderr, "[SF2DIAG] --- chords rendered by the real engine ---\n");
    diag_chord("D-cache ON ");
#ifdef PURRMIDI_SF2_XIP
    SCB_DisableDCache();
    diag_chord("D-cache OFF");
    SCB_EnableDCache();
#endif
    SoundFontSynth_Init();
    SoundFontSynth_ResetStats();
    std::fprintf(stderr, "[SF2DIAG] ===== end of diagnostics =====\n");
}

#else /* host build */

void SoundFontSynth_Profile(void) {}

#endif

} // extern "C"

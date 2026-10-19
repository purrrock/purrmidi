/**
 * @file    soundfont_synth.cpp
 * @brief   SoundFont synth engine implementation using TinySoundFont; the bank lives in QSPI flash (XIP).
 *
 * Threading model (same as the other engines, see synth_engine.cpp)
 * -----------------------------------------------------------------
 * TinySoundFont keeps all of its state (voices, channels) in one structure that is NOT thread
 * safe: tsf_note_on() publishes a voice (playingPreset) before its envelopes, pan and pitch are
 * filled in. MIDI events arrive in the main context while tsf_render_*() runs in the audio
 * context, so the events must never call TinySoundFont directly. They are put into a lock-free
 * SPSC FIFO (producer: main, consumer: audio) and applied at the start of the next render.
 *
 * Sample data
 * -----------
 * The SoundFont bank is stored ONLY in the QSPI flash and read in place: on the target the flash is
 * memory-mapped at 0x90000000 (XIP), and TinySoundFont reads the 16-bit PCM straight from that window
 * (tsf_set_smpl_pcm16()). Presets/instruments/regions are parsed once from the same window and kept
 * in the heap. The SD card is touched only by SF2Loader_Ensure() at power-up, to update the bank in
 * the flash (see sf2_loader.h). On the host (tests, Windows app) sf2_loader_host.cpp provides the same
 * API and the "flash" is the bank file read into RAM.
 */
#include "soundfont_synth.h"
#include "sf2_loader.h"
#include "synth_common.h"
#include "tsf.h"

/* Host build (unit tests, Windows app) or the STM32 target. */
#ifndef SF2_HOST_MODE
#if defined(PURRMIDI_HOST_BUILD) || defined(_WIN32) || defined(WIN32) || defined(__unix__) || defined(__APPLE__)
#define SF2_HOST_MODE 1
#else
#define SF2_HOST_MODE 0
#endif
#endif

#if !SF2_HOST_MODE
#include "stm32h7xx.h"   /* DWT cycle counter for the diagnostics */
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>

/* 1 = collect per-block statistics (render time, voices, clipping) and print them from the main loop. */
#ifndef SF2_DIAG_STATS
#define SF2_DIAG_STATS 1
#endif

/* Порог (в % бюджета блока), после которого гасятся отпущенные голоса, и сколько голосов гасить за блок. */
#ifndef SF2_SHED_THRESHOLD_PCT
#define SF2_SHED_THRESHOLD_PCT 75U
#endif
#ifndef SF2_SHED_VOICES_PER_BLOCK
#define SF2_SHED_VOICES_PER_BLOCK 4
#endif

#define SF_EVENT_FIFO_SIZE 256U   /* power of two */
#define SF_NOTE_ON_FIFO_LIMIT 224U /* slots for Note On / Program / continuous CC; the rest is reserved for Note Off and pedal */
#define SF_MAX_NOTE_REGIONS 8

namespace {

tsf  *g_tsf = nullptr;
bool  g_loaded = false;
bool  g_init_attempted = false;   /* SoundFontSynth_InitSF2() has been run at least once */

/* EV_PROGRAM: смена пресета; индекс пресета (0..65535) передаётся парой байт a (младший) и b (старший). */
enum EventType : uint8_t { EV_NOTE_ON = 1, EV_NOTE_OFF, EV_CC, EV_PROGRAM };

struct Event {
    uint8_t type;
    uint8_t a;
    uint8_t b;
};

/* Event queue, press accounting and emergency-recovery protocol (shared component, see synth_common.h):
 * producer - main, consumer - audio. Note Off, the sustain pedal and All Notes/Sound Off are "critical" and may use
 * the whole FIFO; Note On, Program Change and other CCs are limited to SF_NOTE_ON_FIFO_LIMIT slots so that a flood of
 * notes can never push out a Note Off (which would leave a stuck note). */
synth::NoteInput<Event, SF_EVENT_FIFO_SIZE, SF_NOTE_ON_FIFO_LIMIT> g_input;

/* Pitch wheel: "last value" (0..16383, centre 8192), no queue - losing intermediate values is harmless and the return
 * to centre can never be lost. Written by main, read by the audio context. */
std::atomic<int> g_pitch_wheel{8192};
int              g_pitch_applied = 8192;   /* audio context only */
std::atomic<uint32_t> g_preset_requested{0};   /* последний запрошенный из main индекс пресета */
uint32_t         g_preset_applied = 0;     /* пресет, применённый в аудиоконтексте */

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
    volatile uint32_t voices_shed;     /* отпущенные голоса, ускоренно погашенные защитой от перегрузки */
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
        {
            /* Индекс пресета в банке (а не номер MIDI-программы): в банке могут быть пресеты с одинаковым номером
             * в разных банках, а выбираем мы их по порядку списка. */
            const int idx = (int)ev.a | ((int)ev.b << 8);
            if (idx < tsf_get_presetcount(g_tsf)) {
                tsf_channel_set_presetindex(g_tsf, 0, idx);
                g_preset_applied = (uint32_t)idx;
            }
            break;
        }
        default:
            break;
    }
}

/* tsf_stream over the memory-mapped bank: TinySoundFont parses the metadata (presets, instruments,
 * regions) straight from flash; the PCM chunk is skipped by tsf_load() and read later in place. */
struct BankStream {
    const uint8_t *base;
    uint32_t       size;
    uint32_t       pos;
};
BankStream g_stream;

int bank_read(void *data, void *ptr, unsigned int size)
{
    BankStream *b = static_cast<BankStream *>(data);
    if (b->pos >= b->size) return 0;
    const uint32_t avail = b->size - b->pos;
    const uint32_t n = (size < avail) ? size : avail;
    std::memcpy(ptr, b->base + b->pos, n);
    b->pos += n;
    return (int)n;
}

int bank_skip(void *data, unsigned int count)
{
    BankStream *b = static_cast<BankStream *>(data);
    if ((uint64_t)b->pos + count > b->size) return 0;
    b->pos += count;
    return 1;
}

unsigned int bank_tell(void *data)
{
    return static_cast<BankStream *>(data)->pos;
}

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
    g_input.Reset();
    g_pitch_wheel.store(8192, std::memory_order_relaxed);
    g_pitch_applied = 8192;

    /* One-time boot scenario: QSPI init, optional SD -> flash update, XIP. */
    const uint8_t *bank = nullptr;
    uint32_t bank_size = 0;
    if (!SF2Loader_Ensure() || !SF2Loader_GetBank(&bank, &bank_size)) {
        std::fprintf(stderr, "[SF2] No SoundFont bank in QSPI flash\n");
        return false;
    }

    g_stream.base = bank;
    g_stream.size = bank_size;
    g_stream.pos  = 0;
    struct tsf_stream stream;
    stream.data = &g_stream;
    stream.read = bank_read;
    stream.skip = bank_skip;
    stream.tell = bank_tell;

    std::fprintf(stderr, "[SF2] Bank found (%lu bytes), parsing presets (tsf_load)...\n", (unsigned long)bank_size);
    g_tsf = tsf_load(&stream);
    if (!g_tsf) {
        std::fprintf(stderr, "[SF2] tsf_load() failed. The bank may be invalid, truncated or unsupported.\n");
        return false;
    }

    /* PCM of the 'smpl' chunk is read in place. The offset must be even (int16 alignment) and inside the bank. */
    const uint32_t smpl_off   = tsf_get_smpl_file_offset(g_tsf);
    const uint32_t smpl_total = tsf_get_smpl_sample_count(g_tsf);
    if ((smpl_off & 1U) != 0U || smpl_off >= bank_size) {
        std::fprintf(stderr, "[SF2] Bad smpl chunk offset %lu\n", (unsigned long)smpl_off);
        tsf_close(g_tsf);
        g_tsf = nullptr;
        return false;
    }
    const uint32_t max_samples = (bank_size - smpl_off) / 2U;   /* never read past the end of the bank */
    tsf_set_smpl_pcm16(g_tsf, reinterpret_cast<const short *>(bank + smpl_off),
                       (smpl_total < max_samples) ? smpl_total : max_samples);
    std::fprintf(stderr, "[SF2] SoundFont metadata loaded successfully.\n");

    tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, 48000, SF2_GLOBAL_GAIN_DB);
    tsf_set_max_voices(g_tsf, 32);

    /* MIDI channel 0 must exist before the first note: tsf_channel_note_on() ignores notes
     * for channels that have not been created. */
    tsf_channel_set_presetindex(g_tsf, 0, 0);
    g_preset_requested.store(0, std::memory_order_relaxed);
    g_preset_applied = 0;

    g_loaded = true;
    return true;
}

void SoundFontSynth_Init(void)
{
    /* Try to load the SoundFont only ONCE. Retrying on every engine selection would block the
     * main loop (QSPI init / metadata parsing) each time the user cycles through the instruments
     * when the bank is missing. On the target main() loads it at power-up. */
    if (!g_loaded && !g_tsf && !g_init_attempted) {
        SoundFontSynth_InitSF2();
    }

    if (g_tsf) {
        tsf_reset(g_tsf);
        tsf_channel_set_presetindex(g_tsf, 0, 0);
        g_preset_requested.store(0, std::memory_order_relaxed);
        g_preset_applied = 0;
        tsf_channel_set_pitchwheel(g_tsf, 0, 8192);
    }
    g_input.Reset();
    g_pitch_wheel.store(8192, std::memory_order_relaxed);
    g_pitch_applied = 8192;
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

    Event ev = { EV_NOTE_ON, note, velocity };
    g_input.NoteOn(note, ev);
}

void SoundFontSynth_NoteOff(uint8_t note)
{
    if (!g_loaded || !g_tsf) return;
    if (note > 127) return;

    Event ev = { EV_NOTE_OFF, note, 0 };
    g_input.NoteOff(note, ev);
}

void SoundFontSynth_ControlChange(uint8_t control, uint8_t value)
{
    if (!g_loaded || !g_tsf) return;
    if (control > 127) return;

    Event ev = { EV_CC, control, (uint8_t)(value > 127 ? 127 : value) };
    if (control == 64 || control == 120 || control == 123) {
        g_input.Critical(ev);       /* pedal, All Sound Off, All Notes Off: losing one means stuck notes */
    } else {
        g_input.Soft(ev);           /* volume, expression, reverb...: may be dropped under overload */
    }
}

void SoundFontSynth_PitchBend(int16_t bend)
{
    if (!g_loaded || !g_tsf) return;

    int wheel = (int)bend + 8192;
    if (wheel < 0) wheel = 0;
    if (wheel > 16383) wheel = 16383;
    g_pitch_wheel.store(wheel, std::memory_order_relaxed);
}

void SoundFontSynth_ProgramChange(uint8_t program)
{
    if (!g_loaded || !g_tsf) return;

    SoundFontSynth_SelectPreset(program);
}

uint32_t SoundFontSynth_GetPresetCount(void)
{
    return (g_loaded && g_tsf) ? (uint32_t)tsf_get_presetcount(g_tsf) : 0U;
}

const char *SoundFontSynth_GetPresetName(uint32_t preset_index)
{
    if (!g_loaded || !g_tsf || preset_index >= (uint32_t)tsf_get_presetcount(g_tsf)) return nullptr;
    return tsf_get_presetname(g_tsf, (int)preset_index);
}

void SoundFontSynth_SelectPreset(uint32_t preset_index)
{
    if (!g_loaded || !g_tsf) return;
    if (preset_index >= (uint32_t)tsf_get_presetcount(g_tsf)) return;

    g_preset_requested.store(preset_index, std::memory_order_relaxed);
    Event ev = { EV_PROGRAM, (uint8_t)(preset_index & 0xFFU), (uint8_t)((preset_index >> 8) & 0xFFU) };
    g_input.Soft(ev);
}

uint32_t SoundFontSynth_GetPreset(void)
{
    return g_preset_requested.load(std::memory_order_relaxed);
}

void SoundFontSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames)
{
    if (!g_loaded || !g_tsf) {
        std::memset(buffer, 0, (size_t)num_frames * 2U * sizeof(int16_t));
        return;
    }

    /* A critical event did not fit into the FIFO: a Note Off may have been lost. Silence everything quickly and
     * release the pedal instead of leaving a stuck note. */
    g_input.ServiceRecovery([]() {
        tsf_channel_sounds_off_all(g_tsf, 0);
        tsf_channel_midi_control(g_tsf, 0, 64, 0);
    });

    const int wheel = g_pitch_wheel.load(std::memory_order_relaxed);
    if (wheel != g_pitch_applied) {
        g_pitch_applied = wheel;
        tsf_channel_set_pitchwheel(g_tsf, 0, wheel);
    }

    Event ev;
    while (g_input.Pop(ev)) {
        apply_event(ev);
    }

    /* XIP guard: if the bank window is closed (QSPI switched to indirect mode, see sf2_loader.h), reading the PCM
     * would fault (MemManage on the target). Events above only touch voice bookkeeping in the heap, so they are
     * applied anyway; the voices simply stand still and continue when the window opens again. */
    if (!SF2Loader_BankReadable()) {
        std::memset(buffer, 0, (size_t)num_frames * 2U * sizeof(int16_t));
        return;
    }

    /* Обнуление денормалей: затухающие хвосты голосов (огибающие, ФНЧ) иначе резко замедляют FPU. */
    synth::EnableFlushToZero();

    const uint32_t t0 = diag_cycles();
    render_block(buffer, num_frames);
    const uint32_t dt = diag_cycles() - t0;

#if !SF2_HOST_MODE
    /* Защита от перегрузки: сложный аккорд с педалью даёт десятки голосов, и блок перестаёт укладываться в бюджет
     * (хрипы и пропуски). Если блок занял больше SF2_SHED_THRESHOLD_PCT % бюджета, ускоренно гасим до
     * SF2_SHED_VOICES_PER_BLOCK уже отпущенных голосов с самыми тихими хвостами; звучащие ноты не трогаем. */
    const uint32_t budget_cycles = (SystemCoreClock / 48000U) * num_frames;
    if (dt > (budget_cycles / 100U) * SF2_SHED_THRESHOLD_PCT) {
        const int shed = tsf_shed_releasing_voices(g_tsf, SF2_SHED_VOICES_PER_BLOCK);
#if SF2_DIAG_STATS
        g_diag.voices_shed += (uint32_t)shed;
#else
        (void)shed;
#endif
    }
#endif

#if SF2_DIAG_STATS
    const uint32_t voices = (uint32_t)tsf_active_voice_count(g_tsf);

    g_diag.blocks++;
    g_diag.cycles_sum += dt;
    if (dt > g_diag.cycles_max) g_diag.cycles_max = dt;
#if !SF2_HOST_MODE
    if (dt > budget_cycles) g_diag.over_budget++;
#endif
    g_diag.voices_last = voices;
    if (voices > g_diag.voices_max) g_diag.voices_max = voices;
#endif
}

bool SoundFontSynth_IsLoaded(void)
{
    return g_loaded;
}

uint32_t SoundFontSynth_GetSampleCount(void)
{
    return g_tsf ? tsf_get_smpl_sample_count(g_tsf) : 0U;
}

bool SoundFontSynth_IsAvailable(void)
{
    return g_loaded || !g_init_attempted;
}


/* ------------------------------------------------------------------------------------------------
 * Diagnostics: SoundFontSynth_ReportStats() - one line per second while notes are playing
 * (see soundfont_synth.h)
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
    const uint32_t dropped = g_input.Dropped();

    /* Печатаем только при проблемах: превышен бюджет блока, не хватило голосов, перегрузка
     * микса выше полной шкалы или потеряны события в FIFO. Иначе окно просто сбрасывается. */
    const bool problem = g_diag.over_budget != 0U || g_diag.voice_shortfall != 0U ||
                         g_diag.over_fs_samples != 0U || dropped != g_diag_fifo_dropped_seen;
    if (!problem) {
        const uint32_t keep = g_diag.regions_max;
        SoundFontSynth_ResetStats();
        g_diag.regions_max = keep;
        return;
    }
    const unsigned long avg_us = (unsigned long)((g_diag.cycles_sum / blocks) / us);
    const unsigned long max_us = (unsigned long)(g_diag.cycles_max / us);

    std::fprintf(stderr,
        "[SF2] 1s: render avg=%lu max=%lu us (over budget=%lu of %lu blocks) | voices max=%lu now=%lu"
        " | notes=%lu regions want=%lu started=%lu short=%lu (max/note=%lu) shed_voices=%lu"
        " | mix peak=%lu.%02lu FS, over 1.0 FS: %lu samples in %lu blocks%s | fifo dropped=%lu\n",
        avg_us, max_us, (unsigned long)g_diag.over_budget, (unsigned long)blocks,
        (unsigned long)g_diag.voices_max, (unsigned long)g_diag.voices_last,
        (unsigned long)g_diag.notes_on, (unsigned long)g_diag.regions_wanted, (unsigned long)g_diag.voices_started,
        (unsigned long)g_diag.voice_shortfall, (unsigned long)g_diag.regions_max, (unsigned long)g_diag.voices_shed,
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

} // extern "C"

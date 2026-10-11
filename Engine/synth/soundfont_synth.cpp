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
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>

/* Number of blocks (SF2_BLOCK_SAMPLES samples each) loaded synchronously per voice at NoteOn. */
#ifndef SF2_PRELOAD_BLOCKS_PER_VOICE
#define SF2_PRELOAD_BLOCKS_PER_VOICE 2U
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

/* Audio context: apply one queued MIDI event to TinySoundFont. */
void apply_event(const Event &ev)
{
    switch (ev.type) {
        case EV_NOTE_ON:
            tsf_channel_note_on(g_tsf, 0, (int)ev.a, velocity_to_float(ev.b));
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

    tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, 48000, 0.0f);
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

    preload_note(note, velocity);

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

    tsf_render_short(g_tsf, buffer, (int)num_frames, 0);
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
 * ------------------------------------------------------------------------------------------------ */

static uint32_t sf_cycles_per_us(void)
{
#if SF2_HOST_MODE
    return 1U;
#else
    const uint32_t v = SystemCoreClock / 1000000U;
    return v ? v : 1U;
#endif
}

void SoundFontSynth_PrintStats(void)
{
    SF2Profile p;
    SF2CacheStats s;
    SF2Cache_GetProfile(&p);
    SF2Cache_GetStats(&s);
    if (p.calls == 0U) {
        return;
    }
    const uint32_t us = sf_cycles_per_us();
    auto pct = [&](uint32_t part) { return (unsigned long)((uint64_t)part * 100U / p.calls); };
    auto avg = [](uint32_t total, uint32_t n) { return (unsigned long)(n ? total / n : 0U); };

    std::fprintf(stderr,
        "[SF2] getsample calls=%lu hint=%lu%% search=%lu%% miss=%lu%% | avg cycles hint=%lu search=%lu miss=%lu max=%lu"
        " | avg slots/search=%lu | total in cache=%lu us\n",
        (unsigned long)p.calls, pct(p.hint_hits), pct(p.search_hits), pct(p.misses),
        avg(p.cycles_hint, p.hint_hits), avg(p.cycles_search, p.search_hits), avg(p.cycles_miss, p.misses),
        (unsigned long)p.max_cycles,
        avg(p.search_slots, p.search_hits + p.misses),
        (unsigned long)((p.cycles_hint + p.cycles_search + p.cycles_miss) / us));
    std::fprintf(stderr,
        "[SF2] main loop: process calls=%lu avg=%lu us max=%lu us | card reads=%lu avg=%lu us max=%lu us"
        " | blocks loaded=%lu miss samples=%lu errors=%lu\n",
        (unsigned long)p.proc_calls, avg(p.proc_cycles_total, p.proc_calls) / us, (unsigned long)(p.proc_cycles_max / us),
        (unsigned long)p.read_calls, avg(p.read_cycles_total, p.read_calls) / us, (unsigned long)(p.read_cycles_max / us),
        (unsigned long)s.blocks_loaded, (unsigned long)s.miss_samples, (unsigned long)s.load_errors);
}

#if !SF2_HOST_MODE

namespace {

inline uint32_t now_cycles(void) { return DWT->CYCCNT; }

/* cycles per operation x10, net of the empty loop */
uint32_t net_x10(uint32_t total, uint32_t base, uint32_t n)
{
    return (total > base) ? (uint32_t)(((uint64_t)(total - base) * 10U) / n) : 0U;
}

void print_x10(const char *name, uint32_t x10)
{
    std::fprintf(stderr, "[SF2PROF]   %-34s %lu.%lu cycles\n", name, (unsigned long)(x10 / 10U), (unsigned long)(x10 % 10U));
}

void profile_atomics(void)
{
    static std::atomic<uint32_t> a{0};
    constexpr uint32_t N = 2000;
    volatile uint32_t sink = 0;

    uint32_t t0 = now_cycles();
    for (uint32_t i = 0; i < N; ++i) { __asm volatile("" ::: "memory"); sink = i; }
    const uint32_t base = now_cycles() - t0;

    t0 = now_cycles();
    for (uint32_t i = 0; i < N; ++i) { sink = a.load(std::memory_order_relaxed); __asm volatile("" ::: "memory"); }
    print_x10("atomic load (relaxed)", net_x10(now_cycles() - t0, base, N));

    t0 = now_cycles();
    for (uint32_t i = 0; i < N; ++i) { sink = a.load(std::memory_order_seq_cst); __asm volatile("" ::: "memory"); }
    print_x10("atomic load (seq_cst)", net_x10(now_cycles() - t0, base, N));

    t0 = now_cycles();
    for (uint32_t i = 0; i < N; ++i) { sink = a.fetch_add(1, std::memory_order_relaxed); __asm volatile("" ::: "memory"); }
    print_x10("atomic fetch_add (relaxed)", net_x10(now_cycles() - t0, base, N));

    t0 = now_cycles();
    for (uint32_t i = 0; i < N; ++i) { sink = a.fetch_add(1, std::memory_order_seq_cst); __asm volatile("" ::: "memory"); }
    print_x10("atomic fetch_add (seq_cst)", net_x10(now_cycles() - t0, base, N));
    (void)sink;
}

void profile_getsample(void)
{
    const uint32_t total_blocks = tsf_get_smpl_sample_count(g_tsf) / SF2_BLOCK_SAMPLES;
    if (total_blocks < 300U) {
        std::fprintf(stderr, "[SF2PROF]   (sample data too small for the GetSample test)\n");
        return;
    }

    /* 31 filler blocks first, so that block 3 ends up in the LAST slot: a search then walks all slots. */
    SF2Cache_Reset();
    for (uint32_t k = 0; k < SF2_CACHE_BLOCKS - 1U; ++k) { SF2Cache_PreloadBlock(100U + k); }
    SF2Cache_PreloadBlock(3U);

    const uint32_t S = 3U * SF2_BLOCK_SAMPLES + 10U;
    constexpr uint32_t M = 200;
    volatile int16_t sink = 0;
    int hint = -1;

    uint32_t t0 = now_cycles();
    for (uint32_t i = 0; i < M; ++i) { hint = -1; sink = SF2Cache_GetSample(g_tsf, S, &hint); }
    print_x10("GetSample hit by search (last slot)", (uint32_t)(((uint64_t)(now_cycles() - t0) * 10U) / M));
    std::fprintf(stderr, "[SF2PROF]   (block 3 found in slot %d, SF2_CACHE_BLOCKS=%lu)\n", hint, (unsigned long)SF2_CACHE_BLOCKS);

    hint = (hint >= 0) ? hint : 0;
    t0 = now_cycles();
    for (uint32_t i = 0; i < M; ++i) { sink = SF2Cache_GetSample(g_tsf, S, &hint); }
    print_x10("GetSample hit by hint", (uint32_t)(((uint64_t)(now_cycles() - t0) * 10U) / M));

    /* misses: distinct blocks (each also queues load requests), then the same block repeated */
    uint32_t tm = 0;
    for (uint32_t k = 0; k < 8U; ++k) {
        int h = -1;
        const uint32_t t1 = now_cycles();
        sink = SF2Cache_GetSample(g_tsf, (total_blocks / 2U + k * 7U) * SF2_BLOCK_SAMPLES + 5U, &h);
        tm += now_cycles() - t1;
    }
    print_x10("GetSample miss, new block (+requests)", (uint32_t)(((uint64_t)tm * 10U) / 8U));
    t0 = now_cycles();
    for (uint32_t i = 0; i < M; ++i) { int h = -1; sink = SF2Cache_GetSample(g_tsf, (total_blocks / 2U) * SF2_BLOCK_SAMPLES + 5U, &h); }
    print_x10("GetSample miss, block already requested", (uint32_t)(((uint64_t)(now_cycles() - t0) * 10U) / M));
    (void)sink;

    SF2Cache_Reset();
}

void profile_chord(void)
{
    static int16_t buf[256];
    static const uint8_t notes[] = { 48, 52, 55, 60, 64, 67 };
    static const int counts[] = { 1, 3, 6 };
    const uint32_t us = sf_cycles_per_us();
    constexpr int BLOCKS = 60;

    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); ++c) {
        const int n = counts[c];
        SoundFontSynth_Init();
        SF2Cache_ResetProfile();
        SF2Cache_ResetStats();

        uint32_t t0 = now_cycles();
        for (int k = 0; k < n; ++k) { SoundFontSynth_NoteOn(notes[k], 100); }
        const uint32_t pre = now_cycles() - t0;

        uint32_t first = 0, sum = 0, mx = 0, proc_max = 0;
        int voices = 0;
        for (int b = 0; b < BLOCKS; ++b) {
            t0 = now_cycles();
            SoundFontSynth_FillStereoBuffer(buf, 128);
            const uint32_t d = now_cycles() - t0;
            if (b == 0) { first = d; voices = tsf_active_voice_count(g_tsf); }
            sum += d;
            if (d > mx) mx = d;

            t0 = now_cycles();
            SoundFontSynth_Process();          /* what the main loop does between audio blocks */
            const uint32_t dp = now_cycles() - t0;
            if (dp > proc_max) proc_max = dp;
        }
        std::fprintf(stderr,
            "[SF2PROF] chord n=%d (tsf voices=%d): note-on preload=%lu us | first fill=%lu us, avg=%lu us, max=%lu us"
            " (budget 2666 us) | process max=%lu us\n",
            n, voices, (unsigned long)(pre / us), (unsigned long)(first / us), (unsigned long)(sum / BLOCKS / us),
            (unsigned long)(mx / us), (unsigned long)(proc_max / us));
        SoundFontSynth_PrintStats();
    }
}

} // namespace

void SoundFontSynth_Profile(void)
{
    if (!g_loaded || !g_tsf) {
        std::fprintf(stderr, "[SF2PROF] SoundFont not loaded, skipped\n");
        return;
    }
    std::fprintf(stderr, "[SF2PROF] --- atomic operations (D-cache is off, ARM seq_cst adds DMB barriers) ---\n");
    profile_atomics();
    std::fprintf(stderr, "[SF2PROF] --- SF2Cache_GetSample() ---\n");
    profile_getsample();
    std::fprintf(stderr, "[SF2PROF] --- chord + main loop servicing the cache ---\n");
    profile_chord();
}

#else /* host build */

void SoundFontSynth_Profile(void) {}

#endif

} // extern "C"

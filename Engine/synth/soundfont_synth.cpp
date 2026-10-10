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

#include <atomic>
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

} // extern "C"

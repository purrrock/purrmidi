/**
 * @file    soundfont_synth.cpp
 * @brief   SoundFont synth engine implementation using TinySoundFont & SF2Cache.
 */
#include "soundfont_synth.h"
#include "sf2_cache.h"
#include "tsf.h"

#include <cstring>
#include <cstdio>

namespace {

tsf  *g_tsf = nullptr;
bool  g_loaded = false;

} // namespace

extern "C" {

bool SoundFontSynth_InitSF2(void)
{
    if (g_tsf) {
        tsf_close(g_tsf);
        g_tsf = nullptr;
    }
    g_loaded = false;

    if (!SF2Cache_OpenFile("0:/SNDFNT.SF2")) {
        std::fprintf(stderr, "[SF2] Failed to open SoundFont file; see the attempted host paths above.\n");
        return false;
    }

    struct tsf_stream stream;
    SF2Cache_InitStream(&stream);

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

    g_loaded = true;
    return true;
}

void SoundFontSynth_Init(void)
{
    if (!g_loaded && !g_tsf) {
        SoundFontSynth_InitSF2();
    }

    if (g_tsf) {
        tsf_reset(g_tsf);
        tsf_channel_set_presetnumber(g_tsf, 0, 0, 0);
    }
    SF2Cache_Reset();
}

void SoundFontSynth_NoteOn(uint8_t note, uint8_t velocity)
{
    if (!g_loaded || !g_tsf) return;

    if (velocity == 0) {
        SoundFontSynth_NoteOff(note);
        return;
    }

    float vel_f = (float)velocity / 127.0f;
    tsf_channel_note_on(g_tsf, 0, (int)note, vel_f);
}

void SoundFontSynth_NoteOff(uint8_t note)
{
    if (!g_loaded || !g_tsf) return;
    tsf_channel_note_off(g_tsf, 0, (int)note);
}

void SoundFontSynth_ControlChange(uint8_t control, uint8_t value)
{
    if (!g_loaded || !g_tsf) return;
    tsf_channel_midi_control(g_tsf, 0, (int)control, (int)value);
}

void SoundFontSynth_ProgramChange(uint8_t program)
{
    if (!g_loaded || !g_tsf) return;
    tsf_channel_set_presetnumber(g_tsf, 0, (int)program, 0);
}

void SoundFontSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames)
{
    if (!g_loaded || !g_tsf) {
        std::memset(buffer, 0, (size_t)num_frames * 2U * sizeof(int16_t));
        return;
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

} // extern "C"

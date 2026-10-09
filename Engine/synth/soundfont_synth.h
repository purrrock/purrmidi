/**
 * @file    soundfont_synth.h
 * @brief   TinySoundFont-based SoundFont synthesizer engine for PurrMidi.
 */
#ifndef SOUNDFONT_SYNTH_H
#define SOUNDFONT_SYNTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Load 0:/SNDFNT.SF2 and initialize SoundFont engine. Returns true on success. */
bool SoundFontSynth_InitSF2(void);

/** Reset voices and channel state. Safe to call from main context. */
void SoundFontSynth_Init(void);

void SoundFontSynth_NoteOn(uint8_t note, uint8_t velocity);
void SoundFontSynth_NoteOff(uint8_t note);
void SoundFontSynth_ControlChange(uint8_t control, uint8_t value);
void SoundFontSynth_ProgramChange(uint8_t program);

/** Fill stereo buffer with 48 kHz 16-bit PCM audio (non-blocking). */
void SoundFontSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/** Process pending PCM block requests from SD card. Called from main loop. */
void SoundFontSynth_Process(void);

/** Returns true if SoundFont file was successfully loaded. */
bool SoundFontSynth_IsLoaded(void);

#ifdef __cplusplus
}
#endif

#endif /* SOUNDFONT_SYNTH_H */

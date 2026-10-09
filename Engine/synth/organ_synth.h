#ifndef ORGAN_SYNTH_H
#define ORGAN_SYNTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tonewheel Organ synthesizer adapter for PurrMidi,
 * based on upstream bfreeOrgan2 DSP implementation.
 *
 * Audio format: 48 kHz, signed 16-bit, stereo (interleaved L, R, L, R...).
 *
 * Supported MIDI CCs:
 *   CC20 - Drawbar 16'
 *   CC21 - Drawbar 5 1/3'
 *   CC22 - Drawbar 8'
 *   CC23 - Drawbar 4'
 *   CC24 - Drawbar 2 2/3'
 *   CC25 - Drawbar 2'
 *   CC26 - Drawbar 1 3/5'
 *   CC27 - Drawbar 1 1/3'
 *   CC28 - Drawbar 1'
 *   CC70 - Chorus ON/OFF (>63 ON, <=63 OFF)
 *   CC71 - Chorus Rate
 *   CC72 - Chorus Depth
 *   CC77 - Attack Rate
 *   CC78 - Release Rate
 *   CC120 / CC123 - All Sound Off / All Notes Off
 */

void OrganSynth_Init(void);

void OrganSynth_NoteOn(uint8_t midi_note, uint8_t velocity);
void OrganSynth_NoteOff(uint8_t midi_note);
void OrganSynth_ControlChange(uint8_t control, uint8_t value);

/**
 * Fills buffer with interleaved stereo PCM (48 kHz, int16_t).
 * Real-time safe: called from audio interrupt or host audio thread.
 */
void OrganSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/** Returns number of dropped MIDI events due to FIFO overflow. */
uint32_t OrganSynth_GetDroppedEventCount(void);

#ifdef __cplusplus
}
#endif

#endif /* ORGAN_SYNTH_H */

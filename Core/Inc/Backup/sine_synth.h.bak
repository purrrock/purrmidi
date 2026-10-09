#ifndef SINE_SYNTH_H
#define SINE_SYNTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Одноголосая синусоида с MIDI-интерфейсом (тонкая обёртка над synth.c).
 * Предназначена для проверки аудиотракта и MIDI-цепочки.
 *
 * Монофонический режим, приоритет последней ноты: NoteOff действует только
 * на ту ноту, которая сейчас звучит. Есть короткие линейные атака/релиз
 * (защита от щелчков), педаль сустейна (CC64) и громкость (CC7).
 */

void SineSynth_Init(void);
void SineSynth_NoteOn(uint8_t midi_note, uint8_t velocity);
void SineSynth_NoteOff(uint8_t midi_note);
void SineSynth_ControlChange(uint8_t control, uint8_t value);
void SineSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

#ifdef __cplusplus
}
#endif

#endif /* SINE_SYNTH_H */

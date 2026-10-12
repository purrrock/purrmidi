#ifndef SINE_SYNTH_H
#define SINE_SYNTH_H

#include <stdint.h>

/**
 * @file    sine_synth.h
 * @brief   Одноголосый синусоидальный синтезатор для проверки звукового тракта.
 *
 * Одноголосая синусоида с MIDI-интерфейсом (обёртка над базовым генератором synth.c).
 * Поддерживает приоритет последней нажатой ноты, короткие огибающие огибания
 * атаки и релиза для предотвращения щелчков, а также педаль сустейна (CC 64) и громкость (CC 7).
 */

#ifdef __cplusplus
extern "C" {
#endif

void SineSynth_Init(void);
void SineSynth_NoteOn(uint8_t midi_note, uint8_t velocity);
void SineSynth_NoteOff(uint8_t midi_note);
void SineSynth_ControlChange(uint8_t control, uint8_t value);
void SineSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

#ifdef __cplusplus
}
#endif

#endif /* SINE_SYNTH_H */

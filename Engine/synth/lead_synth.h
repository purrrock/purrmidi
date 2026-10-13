#ifndef LEAD_SYNTH_H
#define LEAD_SYNTH_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file    lead_synth.h
 * @brief   Полифонический субтрактивный синтезатор лидов (Lead) на базе DaisySP.
 *
 * Описание:
 *   Полифонический аналоговый субтрактивный лид-синтезатор (до 8 голосов).
 *   Каждый голос построен по классической субтрактивной схеме:
 *     - Осциллятор с антиалиасингом (daisysp::Oscillator, WAVE_POLYBLEP_SAW);
 *     - Резонансный фильтр нижних частот (daisysp::Svf);
 *     - Огибающие ADSR (daisysp::Adsr) для амплитуды и фильтра.
 *
 *   Поддерживает MIDI Note On/Off, скорость нажатия (velocity), педаль сустейна (CC 64),
 *   управление срезом фильтра (CC 74), резонансом (CC 71) и огибающими (CC 72/73/1).
 */

#ifdef __cplusplus
extern "C" {
#endif

#define LEAD_VOICES 8

void LeadSynth_Init(void);
void LeadSynth_NoteOn(uint8_t midi_note, uint8_t velocity);
void LeadSynth_NoteOff(uint8_t midi_note);
void LeadSynth_ControlChange(uint8_t control, uint8_t value);
void LeadSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);
bool LeadSynth_IsNoteActive(uint8_t midi_note);

#ifdef __cplusplus
}
#endif

#endif /* LEAD_SYNTH_H */

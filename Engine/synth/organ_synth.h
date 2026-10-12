#ifndef ORGAN_SYNTH_H
#define ORGAN_SYNTH_H

#include <stdint.h>

/**
 * @file    organ_synth.h
 * @brief   Адаптер органа Хаммонда (Tonewheel Organ) на базе библиотеки bfreeOrgan2.
 *
 * Формат аудио: 48 кГц, signed 16-bit, стерео с чередованием каналов (L, R, L, R...).
 *
 * Поддерживаемые MIDI Control Change:
 *   CC 20 - Регистр Drawbar 16'
 *   CC 21 - Регистр Drawbar 5 1/3'
 *   CC 22 - Регистр Drawbar 8'
 *   CC 23 - Регистр Drawbar 4'
 *   CC 24 - Регистр Drawbar 2 2/3'
 *   CC 25 - Регистр Drawbar 2'
 *   CC 26 - Регистр Drawbar 1 3/5'
 *   CC 27 - Регистр Drawbar 1 1/3'
 *   CC 28 - Регистр Drawbar 1'
 *   CC 70 - Хорус/Вибрато ВКЛ/ВЫКЛ (>63 ВКЛ, <=63 ВЫКЛ)
 *   CC 71 - Скорость хоруса
 *   CC 72 - Глубина хоруса
 *   CC 77 - Скорость атаки
 *   CC 78 - Скорость релиза
 *   CC 120 - All Sound Off (мгновенное глушение)
 *   CC 123 - All Notes Off (сброс с естественным релизом)
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Инициализация органного движка и сброс состояния. */
void OrganSynth_Init(void);

/** Обработка события Note On. */
void OrganSynth_NoteOn(uint8_t midi_note, uint8_t velocity);

/** Обработка события Note Off. */
void OrganSynth_NoteOff(uint8_t midi_note);

/** Обработка MIDI Control Change (drawbars, chorus, envelopes). */
void OrganSynth_ControlChange(uint8_t control, uint8_t value);

/**
 * Блочное заполнение стереобуфера (L, R, L, R...), 48 кГц, signed 16-bit PCM.
 * Безопасна для реального времени: вызывается из аудиопрерывания или аудиопотока.
 */
void OrganSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/** Возвращает количество потерянных MIDI-событий из-за переполнения FIFO. */
uint32_t OrganSynth_GetDroppedEventCount(void);

#ifdef __cplusplus
}
#endif

#endif /* ORGAN_SYNTH_H */

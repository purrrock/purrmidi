#ifndef DULCIMER_SYNTH_H
#define DULCIMER_SYNTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализация синтезатора Цимбалы / Модальный резонатор (Hammered Dulcimer / Modal Synthesis)
 */
void DulcimerSynth_Init(void);

/**
 * @brief Обработка события нажатия клавиши (MIDI Note On)
 * @param midi_note Номер MIDI-ноты (0..127)
 * @param velocity  Сила нажатия (1..127, 0 трактуется как Note Off)
 */
void DulcimerSynth_NoteOn(uint8_t midi_note, uint8_t velocity);

/**
 * @brief Обработка события отпускания клавиши (MIDI Note Off)
 * @param midi_note Номер отпущенной MIDI-ноты (0..127)
 */
void DulcimerSynth_NoteOff(uint8_t midi_note);

/**
 * @brief Обработка MIDI Control Change (CC)
 * @param cc_num Номер контроллера MIDI CC
 * @param value  Значение контроллера (0..127)
 */
void DulcimerSynth_ControlChange(uint8_t cc_num, uint8_t value);

/**
 * @brief Блочное заполнение стереобуфера PCM (48 кГц, 16 бит)
 * @param buffer      Указатель на выходной стереобуфер (L, R, L, R...)
 * @param num_samples Количество стереокадров для сэмплирования
 */
void DulcimerSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_samples);

#ifdef __cplusplus
}
#endif

#endif /* DULCIMER_SYNTH_H */

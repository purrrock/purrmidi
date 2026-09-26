#ifndef PLUCK_SYNTH_H
#define PLUCK_SYNTH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализация одноголосного синтезатора струны (Karplus-Strong)
 */
void PluckSynth_Init(void);

/**
 * @brief Обработка нажатия клавиши (MIDI Note On)
 * @param midi_note Номер MIDI-ноты (0..127)
 * @param velocity  Сила нажатия (1..127, значение 0 трактуется как Note Off)
 */
void PluckSynth_NoteOn(uint8_t midi_note, uint8_t velocity);

/**
 * @brief Обработка отпускания клавиши (MIDI Note Off)
 * @param midi_note Номер отпущенной MIDI-ноты (0..127)
 */
void PluckSynth_NoteOff(uint8_t midi_note);

/**
 * @brief Обработка MIDI Control Change (CC)
 * @param control Номер контроллера (например, CC1 — демпфирование, CC64 — педаль сустейна, CC74 — затухание)
 * @param value   Значение контроллера (0..127)
 */
void PluckSynth_ControlChange(uint8_t control, uint8_t value);

/**
 * @brief Ручная установка длительности затухания струны (0.0f .. 1.0f)
 */
void PluckSynth_SetDecay(float decay);

/**
 * @brief Ручная установка яркости/демпфирования струны (0.0f .. 1.0f)
 */
void PluckSynth_SetDamp(float damp);

/**
 * @brief Расчёт одного аудиосэмпла (вызывается из прерывания DMA)
 * @return Знаковый 16-битный сэмпл для ЦАП PCM5102A
 */
int16_t PluckSynth_NextSample(void);

/**
 * @brief Блочное заполнение стереобуфера DMA (Left + Right)
 * @param buffer     Указатель на буфер int16_t (размер массива = num_frames * 2)
 * @param num_frames Количество стереопар (фреймов) для расчёта
 */
void PluckSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

#ifdef __cplusplus
}
#endif

#endif /* PLUCK_SYNTH_H */
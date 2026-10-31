#ifndef LEAD_SYNTH_H
#define LEAD_SYNTH_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file    lead_synth.h
 * @brief   Полифонический аналоговый субтрактивный синтезатор лидов (Lead) на базе DaisySP.
 *
 * Описание:
 *   Восьмиголосый аналоговый субтрактивный синтезатор на компонентах DaisySP:
 *     - Осциллятор с антиалиасингом (daisysp::Oscillator, WAVE_POLYBLEP_SAW);
 *     - Резонансный фильтр нижних частот (daisysp::Svf);
 *     - Огибающие ADSR (daisysp::Adsr) для управления амплитудой и частотой среза фильтра.
 *
 * Управление по MIDI Control Change:
 *     - CC 1:  Мод-колесо: ПРИБАВЛЯЕТ яркости (глубина огибающей фильтра растёт с 4000 до 10000 Гц); в нуле тембр не меняется;
 *     - CC 64: Педаль сустейна (Sustain Pedal);
 *     - CC 71: Резонанс фильтра (Filter Cutoff Resonance);
 *     - CC 72: Время затухания огибающей после отпускания клавиши (Release Time);
 *     - CC 73: Время атаки огибающей (Attack Time);
 *     - CC 74: Базовая частота среза фильтра (Filter Cutoff Frequency);
 *     - CC 120: All Sound Off (быстрое гашение всех голосов).
 *
 * Pitch Bend: LeadSynth_PitchBend(), ±2 полутона.
 *
 * Динамика микса:
 *   Сумма голосов нормируется по 1/sqrt(N) со сглаживанием (без скачков уровня при смене числа голосов),
 *   на выходе стоит мягкий ограничитель (tanh) с потолком ниже полной шкалы int16, поэтому аккорды не
 *   упираются в жёсткий срез. Коэффициенты фильтра обновляются с пониженной частотой (каждые 8 сэмплов).
 *
 * Контракт вызовов:
 *   LeadSynth_Init / NoteOn / NoteOff / ControlChange — из контекста main / MIDI-потока;
 *   LeadSynth_FillStereoBuffer — из контекста аудио-прерывания DMA (STM32) или аудио-потока (ПК).
 *   Взаимодействие между потоками осуществляется через lock-free SPSC FIFO буфер событий.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define LEAD_VOICES 8

/**
 * @brief Инициализация или сброс состояния синтезатора Lead.
 */
void LeadSynth_Init(void);

/**
 * @brief Обработка события нажатия ноты (Note On).
 * @param midi_note Номер MIDI-ноты (0..127).
 * @param velocity  Скорость нажатия (1..127, 0 эквивалентно Note Off).
 */
void LeadSynth_NoteOn(uint8_t midi_note, uint8_t velocity);

/**
 * @brief Обработка события отпускания ноты (Note Off).
 * @param midi_note Номер MIDI-ноты (0..127).
 */
void LeadSynth_NoteOff(uint8_t midi_note);

/**
 * @brief Обработка MIDI-контроллеров (Control Change).
 * @param control Номер контроллера (CC).
 * @param value   Значение контроллера (0..127).
 */
void LeadSynth_ControlChange(uint8_t control, uint8_t value);

/**
 * @brief Колесо высоты тона: bend = -8192..+8191 (0 - центр), диапазон ±2 полутона (LEAD_BEND_RANGE).
 * @details Хранится как «последнее значение» (без очереди), положение сглаживается: щелчков нет.
 */
void LeadSynth_PitchBend(int16_t bend);

/**
 * @brief Генерация очередного блока стерео-сэмплов PCM (48 кГц, 16 бит).
 * @param buffer     Указатель на выходной буфер стереокадров (L, R, L, R...).
 * @param num_frames Количество стереокадров для генерации.
 */
void LeadSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/**
 * @brief Проверка активности конкретной ноты.
 * @param midi_note Номер ноты (0..127).
 * @return true если нота активна в одном из голосов, false в противном случае.
 */
bool LeadSynth_IsNoteActive(uint8_t midi_note);

/**
 * @brief Получение количества отброшенных событий Note On из-за переполнения FIFO.
 * @return Счётчик отброшенных событий Note On.
 */
uint32_t LeadSynth_GetDroppedEventsCount(void);

#ifdef __cplusplus
}
#endif

#endif /* LEAD_SYNTH_H */

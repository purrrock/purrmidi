#ifndef SAXOPHONE_SYNTH_H
#define SAXOPHONE_SYNTH_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file    saxophone_synth.h
 * @brief   Полифонический физический модулятор саксофона/кларнета (Saxophone / Clarinet) на основе модели STK.
 *
 * Описание:
 *   Восьмиголосый синтезатор саксофона/кларнета на основе цифрового волновода и физической модели
 *   одинарной трости (Single-Reed Physical Modeling, STK Clarinet / Saxofony):
 *     - Генератор дыхания и турбулентного шума (давление воздуха во рту, турбулентность потока);
 *     - Нелинейный возбудитель (Reed Table f(dP) = offset + slope * dP);
 *     - Акустический резонатор (Bore Delay Line) с цифровым фильтром затухания высоких частот (LPF);
 *     - Параметрический расчёт длины задержки от частоты MIDI-ноты;
 *     - Управление динамикой, жёсткостью трости и давлением выдоха через Velocity и CC.
 *
 * Управление по MIDI Control Change:
 *     - CC 1:  Глубина вибрато (Vibrato Depth);
 *     - CC 2 / CC 11 / CC 73: Интенсивность дыхания / турбулентность шума (Breath Pressure / Noise Turbulence);
 *     - CC 64: Педаль сустейна (Sustain Pedal);
 *     - CC 71: Жесткость трости / Демпфирование резонатора (Reed Stiffness / Bore Damping);
 *     - CC 72: Время затухания дыхания (Release Time);
 *     - CC 74: Яркость / Разперт трости (Brightness / Reed Aperture).
 *
 * Контракт вызовов:
 *   SaxophoneSynth_Init / NoteOn / NoteOff / ControlChange — из контекста main / MIDI-потока;
 *   SaxophoneSynth_FillStereoBuffer — из контекста аудио-прерывания DMA (STM32) или аудио-потока (ПК).
 *   Взаимодействие между потоками осуществляется через lock-free SPSC FIFO буфер событий.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define SAXOPHONE_VOICES 8

/**
 * @brief Инициализация или сброс состояния синтезатора Saxophone.
 */
void SaxophoneSynth_Init(void);

/**
 * @brief Обработка события нажатия ноты (Note On).
 * @param midi_note Номер MIDI-ноты (0..127).
 * @param velocity  Скорость нажатия (1..127, 0 эквивалентно Note Off).
 */
void SaxophoneSynth_NoteOn(uint8_t midi_note, uint8_t velocity);

/**
 * @brief Обработка события отпускания ноты (Note Off).
 * @param midi_note Номер MIDI-ноты (0..127).
 */
void SaxophoneSynth_NoteOff(uint8_t midi_note);

/**
 * @brief Обработка MIDI-контроллеров (Control Change).
 * @param control Номер контроллера (CC).
 * @param value   Значение контроллера (0..127).
 */
void SaxophoneSynth_ControlChange(uint8_t control, uint8_t value);

/**
 * @brief Генерация очередного блока стерео-сэмплов PCM (48 кГц, 16 бит).
 * @param buffer     Указатель на выходной стереобуфер (L, R, L, R...).
 * @param num_frames Количество стереокадров для генерации.
 */
void SaxophoneSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/**
 * @brief Проверка активности конкретной ноты.
 * @param midi_note Номер ноты (0..127).
 * @return true если нота активна в одном из голосов, false в противном случае.
 */
bool SaxophoneSynth_IsNoteActive(uint8_t midi_note);

/**
 * @brief Получение количества отброшенных событий Note On из-за переполнения FIFO.
 * @return Счётчик отброшенных событий Note On.
 */
uint32_t SaxophoneSynth_GetDroppedEventsCount(void);

#ifdef __cplusplus
}
#endif

#endif /* SAXOPHONE_SYNTH_H */

#ifndef FLUTE_SYNTH_H
#define FLUTE_SYNTH_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file    flute_synth.h
 * @brief   Полифонический физический модулятор флейты (Flute) на основе модели STK.
 *
 * Описание:
 *   Восьмиголосый синтезатор флейты на основе цифрового волновода и физической модели STK Flute:
 *     - Генератор дыхания и турбулентного шума (выдох, турбулентность лабиума);
 *     - Нелинейный возбудитель (Jet Delay Line и Jet Table f(x) = x^3 - x);
 *     - Акустический резонатор (Bore Delay Line) с цифровым фильтром затухания высоких частот;
 *     - Параметрический расчёт длины задержки от частоты MIDI-ноты;
 *     - LFO для модуляции вибрато.
 *
 * Управление по MIDI Control Change:
 *     - CC 1:  Глубина вибрато (Vibrato Depth);
 *     - CC 2 / CC 11 / CC 73: Интенсивность дыхания / турбулентность шума (Breath Pressure / Noise Turbulence);
 *     - CC 64: Педаль сустейна (Sustain Pedal);
 *     - CC 71: Демпфирование резонатора стенками трубки (Bore Damping Filter);
 *     - CC 72: Время затухания дыхания (Release Time);
 *     - CC 74: Отношение струйной задержки k_jet (Jet Delay Ratio).
 *
 * Контракт вызовов:
 *   FluteSynth_Init / NoteOn / NoteOff / ControlChange — из контекста main / MIDI-потока;
 *   FluteSynth_FillStereoBuffer — из контекста аудио-прерывания DMA (STM32) или аудио-потока (ПК).
 *   Взаимодействие между потоками осуществляется через lock-free SPSC FIFO буфер событий.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define FLUTE_VOICES 8

/**
 * @brief Инициализация или сброс состояния синтезатора Flute.
 */
void FluteSynth_Init(void);

/**
 * @brief Обработка события нажатия ноты (Note On).
 * @param midi_note Номер MIDI-ноты (0..127).
 * @param velocity  Скорость нажатия (1..127, 0 эквивалентно Note Off).
 */
void FluteSynth_NoteOn(uint8_t midi_note, uint8_t velocity);

/**
 * @brief Обработка события отпускания ноты (Note Off).
 * @param midi_note Номер MIDI-ноты (0..127).
 */
void FluteSynth_NoteOff(uint8_t midi_note);

/**
 * @brief Обработка MIDI-контроллеров (Control Change).
 * @param control Номер контроллера (CC).
 * @param value   Значение контроллера (0..127).
 */
void FluteSynth_ControlChange(uint8_t control, uint8_t value);

/**
 * @brief Генерация очередного блока стерео-сэмплов PCM (48 кГц, 16 бит).
 * @param buffer     Указатель на выходной стереобуфер (L, R, L, R...).
 * @param num_frames Количество стереокадров для генерации.
 */
void FluteSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/**
 * @brief Проверка активности конкретной ноты.
 * @param midi_note Номер ноты (0..127).
 * @return true если нота активна в одном из голосов, false в противном случае.
 */
bool FluteSynth_IsNoteActive(uint8_t midi_note);

/**
 * @brief Получение количества отброшенных событий Note On из-за переполнения FIFO.
 * @return Счётчик отброшенных событий Note On.
 */
uint32_t FluteSynth_GetDroppedEventsCount(void);

#ifdef __cplusplus
}
#endif

#endif /* FLUTE_SYNTH_H */

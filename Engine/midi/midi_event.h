#ifndef MIDI_EVENT_H
#define MIDI_EVENT_H

#include <stdint.h>

/**
 * @file    midi_event.h
 * @brief   Определение универсальной структуры MIDI-события и масок статуса.
 */

/**
 * @brief Структура 3-байтового MIDI-события (Note On/Off, CC, Program Change).
 */
typedef struct
{
    uint8_t status;  /**< Байт статуса (тип команды | MIDI-канал 0..15) */
    uint8_t data1;   /**< Первый байт данных (номер ноты, номер CC, номер программы) */
    uint8_t data2;   /**< Второй байт данных (динамика velocity, значение CC; 0 для Program Change) */
} MIDI_Event_t;

/* Константы типов MIDI-сообщений (старший ниббл байта статуса) */
#define MIDI_STATUS_NOTE_OFF          0x80U
#define MIDI_STATUS_NOTE_ON           0x90U
#define MIDI_STATUS_POLY_KEY_PRESSURE 0xA0U
#define MIDI_STATUS_CONTROL_CHANGE    0xB0U
#define MIDI_STATUS_PROGRAM_CHANGE    0xC0U
#define MIDI_STATUS_CHANNEL_PRESSURE  0xD0U
#define MIDI_STATUS_PITCH_BEND        0xE0U
#define MIDI_STATUS_SYSTEM_EXCLUSIVE  0xF0U

/* Маски для извлечения типа команды и канала */
#define MIDI_STATUS_MASK              0xF0U
#define MIDI_CHANNEL_MASK             0x0FU

#endif /* MIDI_EVENT_H */

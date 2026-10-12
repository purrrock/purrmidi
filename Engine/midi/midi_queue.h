#ifndef MIDI_QUEUE_H
#define MIDI_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include "midi_event.h"

/**
 * @file    midi_queue.h
 * @brief   Потокобезопасная кольцевая очередь MIDI-событий (SPSC lock-free FIFO).
 *
 * Очередь предназначена для передачи входящих MIDI-событий из фонового
 * контекста приёма (USB-прерывание / callback) в главный цикл приложения.
 */

/** Инициализация кольцевого буфера очереди. */
void MIDI_Queue_Init(void);

/**
 * Помещение события в очередь.
 * @param event Указатель на помещаемое MIDI-событие.
 * @return true — событие успешное добавлено; false — очередь переполнена.
 */
bool MIDI_Queue_Push(const MIDI_Event_t *event);

/**
 * Извлечение события из очереди.
 * @param event Указатель на структуру для сохранения извлечённого события.
 * @return true — событие успешно извлечено; false — очередь пуста.
 */
bool MIDI_Queue_Pop(MIDI_Event_t *event);

/** Возвращает суммарное количество потерянных событий из-за переполнения. */
uint32_t MIDI_Queue_GetOverrunCount(void);

#endif /* MIDI_QUEUE_H */

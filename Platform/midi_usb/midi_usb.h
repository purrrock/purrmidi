#ifndef MIDI_USB_H
#define MIDI_USB_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file    midi_usb.h
 * @brief   Модуль приёма USB MIDI сообщений, обработки пакетов и мониторинга связи.
 */

/** Инициализация структуры и состояния USB MIDI приёмника. */
void MIDI_USB_Init(void);

/** Задача фоновой обработки USB MIDI (вызывается в главном цикле). */
void MIDI_USB_Process(void);

/** Проверка текущего статуса подключения USB MIDI клавиатуры. */
bool MIDI_USB_IsConnected(void);

/** Флаг изменения состояния подключения USB (для обновления UI). */
bool MIDI_USB_HasStateChanged(void);

/** Возвращает общее количество завершённых USB-трансферов. */
uint32_t MIDI_USB_GetPacketsCount(void);

/** Возвращает общее количество раскодированных MIDI-событий. */
uint32_t MIDI_USB_GetEventsCount(void);

/** Возвращает количество ошибок приёма пакетов. */
uint32_t MIDI_USB_GetReceiveErrorsCount(void);

/** Возвращает количество перезапусков (re-arm) приёма USB. */
uint32_t MIDI_USB_GetRearmCount(void);

/** Возвращает системный тикующий момент времени последнего корректного пакета (HAL_GetTick). */
uint32_t MIDI_USB_GetLastPacketTick(void);

#endif /* MIDI_USB_H */

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

/** Возвращает количество ошибок приёма пакетов. */
uint32_t MIDI_USB_GetReceiveErrorsCount(void);

#endif /* MIDI_USB_H */

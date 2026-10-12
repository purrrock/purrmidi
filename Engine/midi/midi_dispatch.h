#ifndef MIDI_DISPATCH_H
#define MIDI_DISPATCH_H

#include <stdint.h>
#include "midi_event.h"

/**
 * @file    midi_dispatch.h
 * @brief   Маршрутизатор MIDI-сообщений к активному звуковому движку.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Маршрутизирует новое MIDI-сообщение (Note On/Off, Control Change, Program Change).
 * @param e Указатель на декодированное MIDI-событие.
 */
void MIDI_Dispatch(const MIDI_Event_t *e);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_DISPATCH_H */

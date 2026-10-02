#ifndef MIDI_QUEUE_H
#define MIDI_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "midi_event.h"

void MIDI_Queue_Init(void);
bool MIDI_Queue_Push(const MIDI_Event_t *event);
bool MIDI_Queue_Pop(MIDI_Event_t *event);
uint32_t MIDI_Queue_GetOverrunCount(void);

#endif /* MIDI_QUEUE_H */

#ifndef MIDI_DISPATCH_H
#define MIDI_DISPATCH_H

#include <stdint.h>
#include "midi_event.h"

#ifdef __cplusplus
extern "C" {
#endif

void MIDI_Dispatch(const MIDI_Event_t *e);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_DISPATCH_H */

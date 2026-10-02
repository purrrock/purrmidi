#ifndef MIDI_USB_H
#define MIDI_USB_H

#include <stdint.h>

void MIDI_USB_Init(void);
void MIDI_USB_Process(void);

uint32_t MIDI_USB_GetPacketsCount(void);
uint32_t MIDI_USB_GetEventsCount(void);
uint32_t MIDI_USB_GetReceiveErrorsCount(void);

#endif /* MIDI_USB_H */

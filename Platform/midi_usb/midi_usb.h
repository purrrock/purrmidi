#ifndef MIDI_USB_H
#define MIDI_USB_H

#include <stdint.h>
#include <stdbool.h>

void MIDI_USB_Init(void);
void MIDI_USB_Process(void);

bool MIDI_USB_IsConnected(void);
bool MIDI_USB_HasStateChanged(void);

uint32_t MIDI_USB_GetTransfersCount(void);
uint32_t MIDI_USB_GetPacketsCount(void);
uint32_t MIDI_USB_GetEventsCount(void);
uint32_t MIDI_USB_GetReceiveErrorsCount(void);
uint32_t MIDI_USB_GetRearmCount(void);
uint32_t MIDI_USB_GetLastPacketTick(void);

#endif /* MIDI_USB_H */

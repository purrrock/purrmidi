#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Display_Init(void);

void Display_SetMidiConnected(bool connected);

void Display_SetLastNote(uint8_t note);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
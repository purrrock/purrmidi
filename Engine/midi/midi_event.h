#ifndef MIDI_EVENT_H
#define MIDI_EVENT_H

#include <stdint.h>

typedef struct
{
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} MIDI_Event_t;

/* MIDI status message type constants (command high nibble) */
#define MIDI_STATUS_NOTE_OFF          0x80U
#define MIDI_STATUS_NOTE_ON           0x90U
#define MIDI_STATUS_POLY_KEY_PRESSURE 0xA0U
#define MIDI_STATUS_CONTROL_CHANGE    0xB0U
#define MIDI_STATUS_PROGRAM_CHANGE    0xC0U
#define MIDI_STATUS_CHANNEL_PRESSURE  0xD0U
#define MIDI_STATUS_PITCH_BEND        0xE0U
#define MIDI_STATUS_SYSTEM_EXCLUSIVE  0xF0U

/* Masks */
#define MIDI_STATUS_MASK              0xF0U
#define MIDI_CHANNEL_MASK             0x0FU

#endif /* MIDI_EVENT_H */

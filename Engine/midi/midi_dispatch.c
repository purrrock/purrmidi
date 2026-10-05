#include "midi_dispatch.h"
#include "pluck_synth.h"

void MIDI_Dispatch(const MIDI_Event_t *e)
{
    if (!e) {
        return;
    }

    uint8_t cmd = e->status & MIDI_STATUS_MASK;

    switch (cmd) {
        case MIDI_STATUS_NOTE_ON:
            if (e->data2 > 0) {
                PluckSynth_NoteOn(e->data1, e->data2);
            } else {
                PluckSynth_NoteOff(e->data1);
            }
            break;

        case MIDI_STATUS_NOTE_OFF:
            PluckSynth_NoteOff(e->data1);
            break;

        case MIDI_STATUS_CONTROL_CHANGE:
            PluckSynth_ControlChange(e->data1, e->data2);
            break;

        default:
            break;
    }
}

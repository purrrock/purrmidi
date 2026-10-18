#include "midi_dispatch.h"
#include "synth_engine.h"

void MIDI_Dispatch(const MIDI_Event_t *e)
{
    if (!e) {
        return;
    }

    uint8_t cmd = e->status & MIDI_STATUS_MASK;

    switch (cmd) {
        case MIDI_STATUS_NOTE_ON:
            /* В MIDI-протоколе Note On с velocity == 0 эквивалентен Note Off */
            if (e->data2 > 0) {
                SynthEngine_NoteOn(e->data1, e->data2);
            } else {
                SynthEngine_NoteOff(e->data1);
            }
            break;

        case MIDI_STATUS_NOTE_OFF:
            SynthEngine_NoteOff(e->data1);
            break;

        case MIDI_STATUS_CONTROL_CHANGE:
            SynthEngine_ControlChange(e->data1, e->data2);
            break;

        case MIDI_STATUS_PROGRAM_CHANGE:
            /* data1 = номер программы; спец-номер 127 циклически переключает движок */
            if (e->data1 == 127) {
                SynthEngine_SelectNext();
            } else {
                SynthEngine_ProgramChange(e->data1);
            }
            break;

        case MIDI_STATUS_PITCH_BEND:
            /* 14 бит: data1 = младшие 7 бит, data2 = старшие 7 бит; центр = 0x2000 (8192) -> -8192..+8191 */
            SynthEngine_PitchBend((int16_t)((((int)(e->data2 & 0x7FU) << 7) | (int)(e->data1 & 0x7FU)) - 8192));
            break;

        case MIDI_STATUS_CHANNEL_PRESSURE:
            /* data1 = давление (Channel Aftertouch) */
            SynthEngine_ChannelPressure(e->data1);
            break;

        case MIDI_STATUS_POLY_KEY_PRESSURE:
            /* data1 = нота, data2 = давление (Polyphonic Aftertouch) */
            SynthEngine_PolyPressure(e->data1, e->data2);
            break;

        default:
            break;
    }
}

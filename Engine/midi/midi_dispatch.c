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
            /* data1 = номер программы -> выбор синтезатора */
            if (e->data1 == 127) {
                SynthEngineId cur = SynthEngine_GetCurrent();
                SynthEngineId next = (SynthEngineId)(((uint32_t)cur + 1U) % SYNTH_ENGINE_COUNT);
                SynthEngine_Select(next);
            } else {
                SynthEngine_ProgramChange(e->data1);
            }
            break;

        default:
            break;
    }
}

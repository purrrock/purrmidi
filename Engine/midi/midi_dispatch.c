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
                /* "Следующий инструмент": пропускаем недоступные (SoundFont без SD/файла). */
                SynthEngineId cur = SynthEngine_GetCurrent();
                SynthEngineId next = cur;
                uint32_t i;
                for (i = 0; i < (uint32_t)SYNTH_ENGINE_COUNT; i++) {
                    next = (SynthEngineId)(((uint32_t)next + 1U) % SYNTH_ENGINE_COUNT);
                    if (SynthEngine_IsAvailable(next)) {
                        break;
                    }
                }
                SynthEngine_Select(next);
            } else {
                SynthEngine_ProgramChange(e->data1);
            }
            break;

        default:
            break;
    }
}

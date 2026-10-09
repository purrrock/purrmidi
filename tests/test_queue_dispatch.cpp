#include <iostream>
#include <cassert>
#include <vector>
extern "C" {
#include "midi_queue.h"
}
#include "midi_dispatch.h"
#include "synth_engine.h"

void test_queue_and_dispatch() {
    MIDI_Queue_Init();
    SynthEngine_Init();

    assert(MIDI_Queue_GetOverrunCount() == 0);

    MIDI_Event_t ev_on = {MIDI_STATUS_NOTE_ON, 60, 100};
    bool pushed = MIDI_Queue_Push(&ev_on);
    assert(pushed);

    MIDI_Event_t popped_ev;
    bool popped = MIDI_Queue_Pop(&popped_ev);
    assert(popped);
    assert(popped_ev.status == MIDI_STATUS_NOTE_ON);
    assert(popped_ev.data1 == 60);
    assert(popped_ev.data2 == 100);

    MIDI_Dispatch(&popped_ev);

    std::vector<int16_t> buf(4800 * 2, 0);
    SynthEngine_FillStereoBuffer(buf.data(), 4800);

    bool non_zero = false;
    for (int16_t s : buf) {
        if (s != 0) {
            non_zero = true;
            break;
        }
    }
    assert(non_zero);
}

void test_queue_overrun() {
    MIDI_Queue_Init();
    assert(MIDI_Queue_GetOverrunCount() == 0);

    MIDI_Event_t ev = {MIDI_STATUS_NOTE_ON, 60, 100};

    size_t pushed_count = 0;
    for (size_t i = 0; i < 300; ++i) {
        if (MIDI_Queue_Push(&ev)) {
            pushed_count++;
        }
    }

    assert(pushed_count == 255);
    assert(MIDI_Queue_GetOverrunCount() == 300 - 255);
}

int main() {
    test_queue_and_dispatch();
    test_queue_overrun();
    std::cout << "test_queue_dispatch passed successfully (synth: " << SynthEngine_GetName() << ")." << std::endl;
    return 0;
}

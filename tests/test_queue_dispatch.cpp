#include <iostream>
#include <cassert>
#include <vector>
extern "C" {
#include "midi_queue.h"
}
#include "midi_dispatch.h"
#include "synth_engine.h"

void test_queue_fifo_order() {
    MIDI_Queue_Init();
    assert(MIDI_Queue_GetOverrunCount() == 0);

    MIDI_Event_t e1 = {MIDI_STATUS_NOTE_ON, 60, 100};
    MIDI_Event_t e2 = {MIDI_STATUS_NOTE_OFF, 60, 0};
    MIDI_Event_t e3 = {MIDI_STATUS_PROGRAM_CHANGE, 1, 0};

    assert(MIDI_Queue_Push(&e1));
    assert(MIDI_Queue_Push(&e2));
    assert(MIDI_Queue_Push(&e3));

    MIDI_Event_t out;
    assert(MIDI_Queue_Pop(&out));
    assert(out.status == MIDI_STATUS_NOTE_ON && out.data1 == 60 && out.data2 == 100);

    assert(MIDI_Queue_Pop(&out));
    assert(out.status == MIDI_STATUS_NOTE_OFF && out.data1 == 60 && out.data2 == 0);

    assert(MIDI_Queue_Pop(&out));
    assert(out.status == MIDI_STATUS_PROGRAM_CHANGE && out.data1 == 1 && out.data2 == 0);

    assert(!MIDI_Queue_Pop(&out));
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

void test_dispatch_integration() {
    MIDI_Queue_Init();
    SynthEngine_Init();

    MIDI_Event_t ev_on = {MIDI_STATUS_NOTE_ON, 60, 100};
    MIDI_Dispatch(&ev_on);

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

int main() {
    test_queue_fifo_order();
    test_queue_overrun();
    test_dispatch_integration();
    std::cout << "test_queue_dispatch passed successfully (synth: " << SynthEngine_GetName() << ")." << std::endl;
    return 0;
}

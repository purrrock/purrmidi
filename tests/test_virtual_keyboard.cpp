#include <cassert>
#include <iostream>
#include <vector>
#include "virtual_keyboard.h"

using namespace purrmidi;

static void TestNoteOnNoteOff() {
    VirtualKeyboard vk;
    std::vector<MIDI_Event_t> events;
    vk.SetEventCallback([&](const MIDI_Event_t& ev) {
        events.push_back(ev);
    });

    vk.SetBaseOctave(4);
    vk.SetVelocity(100);

    // Press Z (scancode 0x2C -> C4 = MIDI 60)
    vk.HandleKeyDown(0x5A, 0x2C);
    assert(events.size() == 1);
    assert((events[0].status & MIDI_STATUS_MASK) == MIDI_STATUS_NOTE_ON);
    assert(events[0].data1 == 60);
    assert(events[0].data2 == 100);
    assert(vk.GetActiveNotesCount() == 1);
    assert(vk.IsNoteActive(60));

    // Release Z
    vk.HandleKeyUp(0x5A, 0x2C);
    assert(events.size() == 2);
    assert((events[1].status & MIDI_STATUS_MASK) == MIDI_STATUS_NOTE_OFF);
    assert(events[1].data1 == 60);
    assert(vk.GetActiveNotesCount() == 0);
    assert(!vk.IsNoteActive(60));

    std::cout << "[PASS] TestNoteOnNoteOff\n";
}

static void TestAutoRepeatSuppression() {
    VirtualKeyboard vk;
    std::vector<MIDI_Event_t> events;
    vk.SetEventCallback([&](const MIDI_Event_t& ev) {
        events.push_back(ev);
    });

    vk.SetBaseOctave(4);

    // Initial press Z
    vk.HandleKeyDown(0x5A, 0x2C);
    assert(events.size() == 1);

    // Repeated auto-repeat KeyDown from OS
    vk.HandleKeyDown(0x5A, 0x2C);
    vk.HandleKeyDown(0x5A, 0x2C);
    assert(events.size() == 1); // No duplicate Note On
    assert(vk.GetActiveNotesCount() == 1);

    // Release Z
    vk.HandleKeyUp(0x5A, 0x2C);
    assert(events.size() == 2);
    assert(vk.GetActiveNotesCount() == 0);

    std::cout << "[PASS] TestAutoRepeatSuppression\n";
}

static void TestChords() {
    VirtualKeyboard vk;
    std::vector<MIDI_Event_t> events;
    vk.SetEventCallback([&](const MIDI_Event_t& ev) {
        events.push_back(ev);
    });

    vk.SetBaseOctave(4);

    // Press Z (C4 = 60), S (C#4 = 61), X (D4 = 62)
    vk.HandleKeyDown(0x5A, 0x2C); // Z
    vk.HandleKeyDown(0x53, 0x1F); // S
    vk.HandleKeyDown(0x58, 0x2D); // X

    assert(events.size() == 3);
    assert(vk.GetActiveNotesCount() == 3);
    assert(vk.IsNoteActive(60));
    assert(vk.IsNoteActive(61));
    assert(vk.IsNoteActive(62));

    // Release S (61)
    vk.HandleKeyUp(0x53, 0x1F);
    assert(events.size() == 4);
    assert((events[3].status & MIDI_STATUS_MASK) == MIDI_STATUS_NOTE_OFF);
    assert(events[3].data1 == 61);
    assert(vk.GetActiveNotesCount() == 2);
    assert(!vk.IsNoteActive(61));

    vk.AllNotesOff();
    assert(vk.GetActiveNotesCount() == 0);

    std::cout << "[PASS] TestChords\n";
}

static void TestOctaveShiftHoldingNote() {
    VirtualKeyboard vk;
    std::vector<MIDI_Event_t> events;
    vk.SetEventCallback([&](const MIDI_Event_t& ev) {
        events.push_back(ev);
    });

    vk.SetBaseOctave(4);

    // Press X (D4 = 62)
    vk.HandleKeyDown(0x58, 0x2D);
    assert(events.size() == 1);
    assert(events[0].data1 == 62);

    // Change octave to 5 while holding X
    vk.SetBaseOctave(5);

    // Release X -> must emit Note Off 62 (NOT 74!)
    vk.HandleKeyUp(0x58, 0x2D);
    assert(events.size() == 2);
    assert((events[1].status & MIDI_STATUS_MASK) == MIDI_STATUS_NOTE_OFF);
    assert(events[1].data1 == 62);

    std::cout << "[PASS] TestOctaveShiftHoldingNote\n";
}

static void TestDuplicateReleaseSafety() {
    VirtualKeyboard vk;
    std::vector<MIDI_Event_t> events;
    vk.SetEventCallback([&](const MIDI_Event_t& ev) {
        events.push_back(ev);
    });

    // Releasing a key that was never pressed
    vk.HandleKeyUp(0x5A, 0x2C);
    assert(events.empty());

    // Press and release Z
    vk.HandleKeyDown(0x5A, 0x2C);
    vk.HandleKeyUp(0x5A, 0x2C);
    assert(events.size() == 2);

    // Releasing Z again
    vk.HandleKeyUp(0x5A, 0x2C);
    assert(events.size() == 2); // No extra event

    std::cout << "[PASS] TestDuplicateReleaseSafety\n";
}

static void TestTabProgramChange() {
    VirtualKeyboard vk;
    std::vector<MIDI_Event_t> events;
    vk.SetEventCallback([&](const MIDI_Event_t& ev) {
        events.push_back(ev);
    });

    // Press TAB (scancode 0x0F)
    vk.HandleKeyDown(0x09, 0x0F);
    assert(events.size() == 1);
    assert((events[0].status & MIDI_STATUS_MASK) == MIDI_STATUS_PROGRAM_CHANGE);
    assert(events[0].data1 == 127);

    // Repeated auto-repeat KeyDown for TAB while held
    vk.HandleKeyDown(0x09, 0x0F);
    vk.HandleKeyDown(0x09, 0x0F);
    assert(events.size() == 1); // No duplicate Program Change events

    // Release TAB and press again
    vk.HandleKeyUp(0x09, 0x0F);
    vk.HandleKeyDown(0x09, 0x0F);
    assert(events.size() == 2);

    std::cout << "[PASS] TestTabProgramChange\n";
}

static void TestOctaveKeys() {
    VirtualKeyboard vk;
    vk.SetBaseOctave(4);

    // Press [ (scancode 0x1A) -> octave down to 3
    vk.HandleKeyDown(0xDB, 0x1A);
    assert(vk.GetBaseOctave() == 3);

    // Auto-repeat KeyDown for [ while held
    vk.HandleKeyDown(0xDB, 0x1A);
    assert(vk.GetBaseOctave() == 3); // Octave stays 3

    // Release [
    vk.HandleKeyUp(0xDB, 0x1A);

    // Press ] (scancode 0x1B) -> octave up to 4
    vk.HandleKeyDown(0xDD, 0x1B);
    assert(vk.GetBaseOctave() == 4);

    std::cout << "[PASS] TestOctaveKeys\n";
}

static void TestFocusLossAndAllNotesOff() {
    VirtualKeyboard vk;
    std::vector<MIDI_Event_t> events;
    vk.SetEventCallback([&](const MIDI_Event_t& ev) {
        events.push_back(ev);
    });

    vk.SetBaseOctave(4);

    // Press Z and X
    vk.HandleKeyDown(0x5A, 0x2C); // Z -> C4 (60)
    vk.HandleKeyDown(0x58, 0x2D); // X -> D4 (62)
    assert(events.size() == 2);
    assert(vk.GetActiveNotesCount() == 2);

    // Focus lost -> AllNotesOff()
    vk.AllNotesOff();
    assert(events.size() == 4); // Should emit 2 Note Offs
    assert((events[2].status & MIDI_STATUS_MASK) == MIDI_STATUS_NOTE_OFF);
    assert((events[3].status & MIDI_STATUS_MASK) == MIDI_STATUS_NOTE_OFF);
    assert(vk.GetActiveNotesCount() == 0);

    // Release keys that were cleared during focus loss -> should NOT emit extra Note Offs
    vk.HandleKeyUp(0x5A, 0x2C);
    vk.HandleKeyUp(0x58, 0x2D);
    assert(events.size() == 4);

    // Press Z again after focus restored -> should emit Note On 60
    vk.HandleKeyDown(0x5A, 0x2C);
    assert(events.size() == 5);
    assert((events[4].status & MIDI_STATUS_MASK) == MIDI_STATUS_NOTE_ON);
    assert(events[4].data1 == 60);

    vk.HandleKeyUp(0x5A, 0x2C);
    assert(events.size() == 6);
    assert((events[5].status & MIDI_STATUS_MASK) == MIDI_STATUS_NOTE_OFF);

    std::cout << "[PASS] TestFocusLossAndAllNotesOff\n";
}

int main() {
    TestNoteOnNoteOff();
    TestAutoRepeatSuppression();
    TestChords();
    TestOctaveShiftHoldingNote();
    TestDuplicateReleaseSafety();
    TestTabProgramChange();
    TestOctaveKeys();
    TestFocusLossAndAllNotesOff();

    std::cout << "All VirtualKeyboard host tests passed successfully!\n";
    return 0;
}

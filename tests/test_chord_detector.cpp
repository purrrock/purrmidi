#include "chord_detector.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

/*
 * Simulated active note tracking stack as used in main.c
 */
struct NoteTracker {
    uint8_t active_notes[128];
    uint8_t active_note_stack[128];
    uint16_t active_note_count;

    NoteTracker() : active_note_count(0) {
        std::memset(active_notes, 0, sizeof(active_notes));
        std::memset(active_note_stack, 0, sizeof(active_note_stack));
    }

    void note_on(uint8_t note, uint8_t velocity) {
        if (velocity == 0) {
            note_off(note);
            return;
        }
        if (note < 128 && active_notes[note] == 0) {
            active_notes[note] = 1;
            active_note_stack[active_note_count] = note;
            active_note_count++;
        }
    }

    void note_off(uint8_t note) {
        if (note < 128 && active_notes[note] != 0) {
            active_notes[note] = 0;
            for (uint16_t i = 0; i < active_note_count; i++) {
                if (active_note_stack[i] == note) {
                    for (uint16_t j = i; j < active_note_count - 1; j++) {
                        active_note_stack[j] = active_note_stack[j + 1];
                    }
                    break;
                }
            }
            if (active_note_count > 0) {
                active_note_count--;
            }
        }
    }

    uint8_t get_last_active_note() const {
        return (active_note_count > 0) ? active_note_stack[active_note_count - 1] : 0;
    }
};

static void set_notes(uint8_t *active_notes, uint16_t &count, const std::vector<uint8_t> &notes)
{
    std::memset(active_notes, 0, 128);
    count = 0;
    for (uint8_t note : notes)
    {
        if (note < 128 && active_notes[note] == 0)
        {
            active_notes[note] = 1;
            count++;
        }
    }
}

static void test_case(const std::vector<uint8_t> &notes, bool expected_detected, const char *expected_name)
{
    uint8_t active_notes[128];
    uint16_t active_note_count = 0;
    set_notes(active_notes, active_note_count, notes);

    char name[32] = {0};
    bool detected = ChordDetector_Detect(active_notes, active_note_count, name, sizeof(name));

    assert(detected == expected_detected);
    if (expected_detected)
    {
        assert(std::strcmp(name, expected_name) == 0);
    }
}

static void test_note_off_regression()
{
    NoteTracker tracker;

    /* 1. C4 ON, E4 ON, E4 OFF => remaining C4 */
    tracker.note_on(60, 100); // C4
    assert(tracker.get_last_active_note() == 60);
    assert(tracker.active_note_count == 1);

    tracker.note_on(64, 100); // E4
    assert(tracker.get_last_active_note() == 64);
    assert(tracker.active_note_count == 2);

    tracker.note_off(64); // E4 OFF
    assert(tracker.get_last_active_note() == 60); // Must be C4, NOT E4!
    assert(tracker.active_note_count == 1);

    /* 2. C4 ON, E4 ON, C4 OFF => remaining E4 */
    tracker.note_off(60);
    tracker.note_on(60, 100); // C4
    tracker.note_on(64, 100); // E4
    tracker.note_off(60);     // C4 OFF
    assert(tracker.get_last_active_note() == 64); // Must be E4
    assert(tracker.active_note_count == 1);

    /* 3. C4 ON, E4 ON, G4 ON, G4 OFF => remaining C4 + E4 (active count 2, last note E4) */
    tracker.note_off(64);
    tracker.note_on(60, 100); // C4
    tracker.note_on(64, 100); // E4
    tracker.note_on(67, 100); // G4 (3 notes -> chord C)
    assert(tracker.active_note_count == 3);

    char chord[16] = {0};
    assert(ChordDetector_Detect(tracker.active_notes, tracker.active_note_count, chord, sizeof(chord)) == true);
    assert(std::strcmp(chord, "C") == 0);

    tracker.note_off(67); // G4 OFF
    assert(tracker.active_note_count == 2);
    assert(tracker.get_last_active_note() == 64); // Last active note is E4
    assert(ChordDetector_Detect(tracker.active_notes, tracker.active_note_count, chord, sizeof(chord)) == false);

    /* 4. Note On with velocity = 0 behaves as Note Off */
    tracker.note_on(60, 0); // Release C4 via velocity 0
    assert(tracker.active_note_count == 1);
    assert(tracker.get_last_active_note() == 64);

    tracker.note_on(64, 0); // Release E4 via velocity 0
    assert(tracker.active_note_count == 0);
    assert(tracker.get_last_active_note() == 0);
}

int main()
{
    /* 0 notes -> blank/no chord */
    test_case({}, false, "");

    /* 1 or 2 notes -> no chord detected by ChordDetector_Detect */
    test_case({60}, false, "");
    test_case({60, 64}, false, "");

    /* C4 E4 G4 -> C */
    test_case({60, 64, 67}, true, "C");

    /* E4 G4 C5 -> C/E */
    test_case({64, 67, 72}, true, "C/E");

    /* G3 C4 E4 -> C/G */
    test_case({55, 60, 64}, true, "C/G");

    /* A3 C4 E4 -> Am */
    test_case({57, 60, 64}, true, "Am");

    /* C4 E4 A4 -> Am/C */
    test_case({60, 64, 69}, true, "Am/C");

    /* E3 A3 C4 -> Am/E */
    test_case({52, 57, 60}, true, "Am/E");

    /* G3 B3 D4 -> G */
    test_case({55, 59, 62}, true, "G");

    /* B3 D4 G4 -> G/B */
    test_case({59, 62, 67}, true, "G/B");

    /* D4 G4 B4 -> G/D */
    test_case({62, 67, 71}, true, "G/D");

    /* C E G B -> Cmaj7 */
    test_case({60, 64, 67, 71}, true, "Cmaj7");

    /* A C E G -> Am7 */
    test_case({57, 60, 64, 67}, true, "Am7");

    /* B D F A -> Bm7b5 */
    test_case({59, 62, 65, 69}, true, "Bm7b5");

    /* Octave duplication tests:
     * C3 C4 E4 G4 -> C
     * E3 C4 G4    -> C/E
     * G3 C4 E4 G4 -> C/G
     */
    test_case({48, 60, 64, 67}, true, "C");
    test_case({52, 60, 67}, true, "C/E");
    test_case({55, 60, 64, 67}, true, "C/G");

    /* Regression tests for Note Off & Note On velocity=0 active note tracking */
    test_note_off_regression();

    std::cout << "All chord_detector and note_off regression tests passed successfully!" << std::endl;
    return 0;
}

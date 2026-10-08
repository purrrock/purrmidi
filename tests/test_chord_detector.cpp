#include "chord_detector.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

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

    std::cout << "All chord_detector tests passed successfully!" << std::endl;
    return 0;
}

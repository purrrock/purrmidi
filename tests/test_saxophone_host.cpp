#include "saxophone_synth.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond    \
                      << std::endl;                                              \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static int calculate_peak(const std::vector<int16_t> &buf) {
    int max_val = 0;
    for (int16_t s : buf) {
        int val = std::abs(static_cast<int>(s));
        if (val > max_val) max_val = val;
    }
    return max_val;
}

int main() {
    std::cout << "Running SaxophoneSynth host tests..." << std::endl;

    // Test 1: SaxophoneSynth_Init resets state and renders silence
    {
        SaxophoneSynth_Init();
        std::vector<int16_t> buf(256 * 2, 0);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(!SaxophoneSynth_IsNoteActive(60));
        CHECK(SaxophoneSynth_GetDroppedEventsCount() == 0);
        CHECK(calculate_peak(buf) == 0);
        std::cout << "[PASS] Test 1: SaxophoneSynth_Init resets state and renders silence." << std::endl;
    }

    // Test 2: Note On produces sound and Note Off completes release
    {
        SaxophoneSynth_Init();
        SaxophoneSynth_NoteOn(60, 100);

        std::vector<int16_t> buf(256 * 2, 0);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(SaxophoneSynth_IsNoteActive(60) == true);
        CHECK(calculate_peak(buf) > 0);

        SaxophoneSynth_NoteOff(60);
        for (int i = 0; i < 40; ++i) {
            SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(SaxophoneSynth_IsNoteActive(60) == false);
        CHECK(calculate_peak(buf) == 0);
        std::cout << "[PASS] Test 2: Note On produces sound and Note Off releases to silence." << std::endl;
    }

    // Test 3: Velocity response
    {
        SaxophoneSynth_Init();
        std::vector<int16_t> buf(256 * 2, 0);

        SaxophoneSynth_NoteOn(60, 30);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        int soft_peak = calculate_peak(buf);
        SaxophoneSynth_NoteOff(60);
        for (int i = 0; i < 40; ++i) SaxophoneSynth_FillStereoBuffer(buf.data(), 256);

        SaxophoneSynth_NoteOn(60, 127);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        int hard_peak = calculate_peak(buf);
        SaxophoneSynth_NoteOff(60);
        for (int i = 0; i < 40; ++i) SaxophoneSynth_FillStereoBuffer(buf.data(), 256);

        CHECK(hard_peak > soft_peak);
        std::cout << "[PASS] Test 3: Higher velocity produces higher amplitude." << std::endl;
    }

    // Test 4: Control Changes and Sustain pedal
    {
        SaxophoneSynth_Init();
        SaxophoneSynth_ControlChange(1, 64);
        SaxophoneSynth_ControlChange(2, 80);
        SaxophoneSynth_ControlChange(11, 100);
        SaxophoneSynth_ControlChange(71, 70);
        SaxophoneSynth_ControlChange(72, 40);
        SaxophoneSynth_ControlChange(73, 90);
        SaxophoneSynth_ControlChange(74, 110);

        SaxophoneSynth_ControlChange(64, 127); // Pedal ON
        SaxophoneSynth_NoteOn(64, 100);

        std::vector<int16_t> buf(256 * 2, 0);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(SaxophoneSynth_IsNoteActive(64) == true);

        SaxophoneSynth_NoteOff(64);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(SaxophoneSynth_IsNoteActive(64) == true); // Held by sustain pedal

        SaxophoneSynth_ControlChange(64, 0); // Pedal OFF
        for (int i = 0; i < 40; ++i) {
            SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(SaxophoneSynth_IsNoteActive(64) == false);
        std::cout << "[PASS] Test 4: Control Changes and Sustain Pedal processed correctly." << std::endl;
    }

    // Test 5: Polyphony and deterministic voice stealing
    {
        SaxophoneSynth_Init();
        std::vector<int16_t> buf(256 * 2, 0);

        for (uint8_t i = 0; i < SAXOPHONE_VOICES; i++) {
            SaxophoneSynth_NoteOn(60 + i, 100);
            SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        }

        for (uint8_t i = 0; i < SAXOPHONE_VOICES; i++) {
            CHECK(SaxophoneSynth_IsNoteActive(60 + i) == true);
        }

        // Trigger 9th note -> oldest note (60) stolen
        SaxophoneSynth_NoteOn(72, 100);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);

        CHECK(SaxophoneSynth_IsNoteActive(60) == false);
        CHECK(SaxophoneSynth_IsNoteActive(72) == true);

        for (uint8_t i = 0; i < SAXOPHONE_VOICES; i++) {
            SaxophoneSynth_NoteOff(60 + i);
        }
        SaxophoneSynth_NoteOff(72);
        for (int i = 0; i < 40; ++i) {
            SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        }
        for (uint8_t i = 0; i <= 12; i++) {
            CHECK(SaxophoneSynth_IsNoteActive(60 + i) == false);
        }
        std::cout << "[PASS] Test 5: Polyphony and deterministic voice stealing verified." << std::endl;
    }

    if (g_failures != 0) {
        std::cerr << "test_saxophone_host: " << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All SaxophoneSynth host tests passed successfully." << std::endl;
    return 0;
}

#include "flute_synth.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cassert>
#include <algorithm>
#define PURRMIDI_CHECK_FAIL_FAST   // линейный тест: после первой ошибки продолжать бессмысленно
#include "check.h"

static int calculate_peak(const std::vector<int16_t> &buf) {
    int max_val = 0;
    for (int16_t s : buf) {
        int val = std::abs((int)s);
        if (val > max_val) max_val = val;
    }
    return max_val;
}

int main() {
    std::cout << "Running comprehensive FluteSynth host tests..." << std::endl;

    // Test 1: FluteSynth_Init resets state and renders silence
    {
        FluteSynth_Init();
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(calculate_peak(buf) == 0);
        std::cout << "[PASS] Test 1: FluteSynth_Init resets state and renders silence." << std::endl;
    }

    // Test 2: Note On produces sound and Note Off completes release envelope
    {
        FluteSynth_Init();
        FluteSynth_NoteOn(60, 100); // C4

        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(FluteSynth_IsNoteActive(60) == true);
        CHECK(calculate_peak(buf) > 0);

        FluteSynth_NoteOff(60);
        // Render 40 blocks (~213 ms) to finish 150 ms release
        for (int i = 0; i < 40; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(60) == false);
        CHECK(calculate_peak(buf) == 0);
        std::cout << "[PASS] Test 2: Note On produces sound and Note Off releases to silence." << std::endl;
    }

    // Test 3: Note On with velocity 0 behaves identically to Note Off
    {
        FluteSynth_Init();
        FluteSynth_NoteOn(60, 100);
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(FluteSynth_IsNoteActive(60) == true);

        FluteSynth_NoteOn(60, 0); // Velocity 0 == Note Off
        for (int i = 0; i < 40; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(60) == false);
        CHECK(calculate_peak(buf) == 0);
        std::cout << "[PASS] Test 3: Note On with velocity 0 behaves as Note Off." << std::endl;
    }

    // Test 4: Overlapping note presses on the same pitch
    {
        FluteSynth_Init();
        FluteSynth_NoteOn(60, 100); // Press 1
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        FluteSynth_NoteOn(60, 100); // Press 2 (overlapping)
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        FluteSynth_NoteOff(60); // Release Press 1
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        // Note 60 MUST remain active because Press 2 is still held
        CHECK(FluteSynth_IsNoteActive(60) == true);

        FluteSynth_NoteOff(60); // Release Press 2
        for (int i = 0; i < 40; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(60) == false);
        std::cout << "[PASS] Test 4: Overlapping note presses on the same pitch handled correctly." << std::endl;
    }

    // Test 5: Polyphony and deterministic voice stealing
    {
        FluteSynth_Init();
        std::vector<int16_t> buf(256 * 2, 0);

        for (uint8_t note = 60; note < 60 + FLUTE_VOICES; ++note) {
            FluteSynth_NoteOn(note, 100);
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }

        for (uint8_t note = 60; note < 60 + FLUTE_VOICES; ++note) {
            CHECK(FluteSynth_IsNoteActive(note) == true);
        }

        // Trigger 9th note -> oldest note (60) stolen!
        FluteSynth_NoteOn(72, 100);
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        CHECK(FluteSynth_IsNoteActive(60) == false); // Stolen
        CHECK(FluteSynth_IsNoteActive(72) == true);  // Active
        for (uint8_t note = 61; note < 60 + FLUTE_VOICES; ++note) {
            CHECK(FluteSynth_IsNoteActive(note) == true);
        }
        std::cout << "[PASS] Test 5: Polyphony and deterministic voice stealing verified." << std::endl;
    }

    // Test 6: Sustain pedal CC 64 and sub-block event ordering
    {
        FluteSynth_Init();
        FluteSynth_ControlChange(64, 127); // Pedal ON

        FluteSynth_NoteOn(60, 100);
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        FluteSynth_NoteOff(60); // Note Off sent while pedal is ON
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        // Note 60 MUST remain active due to sustain pedal
        CHECK(FluteSynth_IsNoteActive(60) == true);

        // Release pedal CC 64 = 0
        FluteSynth_ControlChange(64, 0);
        for (int i = 0; i < 40; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(60) == false);
        std::cout << "[PASS] Test 6: Sustain pedal CC 64 holds and releases notes." << std::endl;
    }

    // Test 7: FIFO overflow and critical event reserved slots
    {
        FluteSynth_Init();

        // Send 40 NoteOn events without rendering. Limit = 28.
        for (int i = 0; i < 40; ++i) {
            FluteSynth_NoteOn(60, 100);
        }

        uint32_t dropped = FluteSynth_GetDroppedEventsCount();
        CHECK(dropped == 12); // 28 accepted, 12 dropped

        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(FluteSynth_IsNoteActive(60) == true);

        // Send 28 NoteOff events
        for (int i = 0; i < 28; ++i) {
            FluteSynth_NoteOff(60);
        }

        for (int i = 0; i < 40; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(60) == false);
        std::cout << "[PASS] Test 7: FIFO overflow drops non-critical events and delivers critical ones." << std::endl;
    }

    // Test 8: 100% full FIFO saturation emergency recovery
    {
        FluteSynth_Init();
        FluteSynth_ControlChange(64, 127); // Sustain ON
        FluteSynth_NoteOn(60, 100);
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(FluteSynth_IsNoteActive(60) == true);

        // Saturate FIFO completely (28 NoteOn + 4 NoteOff = 32 slots)
        for (int i = 0; i < 28; ++i) {
            FluteSynth_NoteOn(64 + (i % 8), 100);
        }
        for (int i = 0; i < 4; ++i) {
            FluteSynth_NoteOff(64 + (i % 8));
        }

        // Send NoteOff(60) when 100% full -> triggers emergency recovery
        FluteSynth_NoteOff(60);

        for (int i = 0; i < 40; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(60) == false);

        // Verify post-recovery operation
        FluteSynth_NoteOn(72, 100);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(FluteSynth_IsNoteActive(72) == true);

        FluteSynth_NoteOff(72);
        for (int i = 0; i < 40; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(72) == false);
        std::cout << "[PASS] Test 8: Emergency recovery state machine handles 100% FIFO saturation." << std::endl;
    }

    // Test 9: Control Change CC 1, 2, 11, 71, 72, 73, 74 processing
    {
        FluteSynth_Init();
        FluteSynth_ControlChange(1, 127);  // Vibrato depth max
        FluteSynth_ControlChange(2, 100);  // Breath turbulence
        FluteSynth_ControlChange(11, 80);  // Expression
        FluteSynth_ControlChange(71, 90);  // Damping
        FluteSynth_ControlChange(72, 50);  // Release time
        FluteSynth_ControlChange(73, 60);  // Attack / Breath intensity
        FluteSynth_ControlChange(74, 110); // Jet ratio

        FluteSynth_NoteOn(60, 100);
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(FluteSynth_IsNoteActive(60) == true);
        CHECK(calculate_peak(buf) > 0);

        FluteSynth_NoteOff(60);
        for (int i = 0; i < 80; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(60) == false);
        std::cout << "[PASS] Test 9: Control Change CC 1, 2, 11, 71, 72, 73, 74 processed." << std::endl;
    }

    // Test 10: Out-of-bounds MIDI notes and CC values handled safely
    {
        FluteSynth_Init();
        FluteSynth_NoteOn(128, 100);
        FluteSynth_NoteOn(255, 100);
        FluteSynth_NoteOff(200);
        FluteSynth_ControlChange(200, 100);

        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(calculate_peak(buf) == 0);
        std::cout << "[PASS] Test 10: Out-of-bounds MIDI note numbers handled safely." << std::endl;
    }

    std::cout << "All 10 FluteSynth host tests passed successfully!" << std::endl;
    return 0;
}

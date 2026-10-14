#include "flute_synth.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

static int calculate_peak(const std::vector<int16_t> &buf) {
    int max_val = 0;
    for (int16_t s : buf) {
        int val = std::abs((int)s);
        if (val > max_val) max_val = val;
    }
    return max_val;
}

int main() {
    std::cout << "Running FluteSynth host tests..." << std::endl;

    // Test 1: FluteSynth_Init resets state and renders silence
    {
        FluteSynth_Init();
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(calculate_peak(buf) == 0);
        std::cout << "[PASS] Test 1: FluteSynth_Init resets state and renders silence." << std::endl;
    }

    // Test 2: Note On produces sound
    {
        FluteSynth_Init();
        FluteSynth_NoteOn(60, 100); // C4

        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(FluteSynth_IsNoteActive(60) == true);
        CHECK(calculate_peak(buf) > 0);
        std::cout << "[PASS] Test 2: Note On produces sound output." << std::endl;
    }

    // Test 3: Note Off decays sound
    {
        FluteSynth_Init();
        FluteSynth_NoteOn(60, 100);
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        FluteSynth_NoteOff(60);
        // Render enough frames for default release envelope (150ms) to finish
        for (int i = 0; i < 40; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(60) == false);
        CHECK(calculate_peak(buf) == 0);
        std::cout << "[PASS] Test 3: Note Off decays sound to silence." << std::endl;
    }

    // Test 4: Polyphony and voice stealing
    {
        FluteSynth_Init();
        std::vector<int16_t> buf(256 * 2, 0);

        for (uint8_t note = 60; note < 60 + FLUTE_VOICES; ++note) {
            FluteSynth_NoteOn(note, 100);
        }
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        for (uint8_t note = 60; note < 60 + FLUTE_VOICES; ++note) {
            CHECK(FluteSynth_IsNoteActive(note) == true);
        }

        // Steal voice by playing an extra note
        FluteSynth_NoteOn(72, 100);
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        CHECK(FluteSynth_IsNoteActive(72) == true);
        CHECK(calculate_peak(buf) > 0);
        std::cout << "[PASS] Test 4: Polyphony and voice stealing work properly." << std::endl;
    }

    // Test 5: Control Change processing (vibrato, breath, damping, release, jet ratio, sustain pedal)
    {
        FluteSynth_Init();
        FluteSynth_ControlChange(1, 100);  // Mod wheel -> vibrato
        FluteSynth_ControlChange(2, 80);   // Breath turbulence
        FluteSynth_ControlChange(71, 60);  // Damping
        FluteSynth_ControlChange(72, 30);  // Release (~370ms)
        FluteSynth_ControlChange(74, 90);  // Jet ratio
        FluteSynth_ControlChange(64, 127); // Sustain pedal ON

        FluteSynth_NoteOn(60, 100);
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        FluteSynth_NoteOff(60); // Note Off sent, but held by sustain pedal
        FluteSynth_FillStereoBuffer(buf.data(), 256);

        CHECK(FluteSynth_IsNoteActive(60) == true);
        CHECK(calculate_peak(buf) > 0);

        // Release sustain pedal
        FluteSynth_ControlChange(64, 0);
        for (int i = 0; i < 80; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(60) == false);
        std::cout << "[PASS] Test 5: Control Change (vibrato, turbulence, damping, release, jet ratio, sustain) processed." << std::endl;
    }

    // Test 6: Velocity 0 treated as Note Off
    {
        FluteSynth_Init();
        FluteSynth_NoteOn(64, 100);
        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(FluteSynth_IsNoteActive(64) == true);

        FluteSynth_NoteOn(64, 0); // velocity 0 == Note Off
        for (int i = 0; i < 40; ++i) {
            FluteSynth_FillStereoBuffer(buf.data(), 256);
        }
        CHECK(FluteSynth_IsNoteActive(64) == false);
        std::cout << "[PASS] Test 6: Velocity 0 treated as Note Off." << std::endl;
    }

    // Test 7: Out-of-bounds MIDI note numbers handled safely
    {
        FluteSynth_Init();
        FluteSynth_NoteOn(128, 100);
        FluteSynth_NoteOn(255, 100);
        FluteSynth_NoteOff(200);
        FluteSynth_ControlChange(200, 100);

        std::vector<int16_t> buf(256 * 2, 0);
        FluteSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(calculate_peak(buf) == 0);
        std::cout << "[PASS] Test 7: Out-of-bounds MIDI notes handled safely." << std::endl;
    }

    std::cout << "All FluteSynth host tests passed successfully!" << std::endl;
    return 0;
}

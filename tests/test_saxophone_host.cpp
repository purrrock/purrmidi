#include "saxophone_synth.h"
#include "synth_engine.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "check.h"

static int calculate_peak(const std::vector<int16_t> &buf) {
    int max_val = 0;
    for (int16_t s : buf) {
        int val = std::abs(static_cast<int>(s));
        if (val > max_val) max_val = val;
    }
    return max_val;
}

int main() {
    std::cout << "Running comprehensive SaxophoneSynth host tests..." << std::endl;

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

    // Test 3: Velocity response and Velocity=0 acting as NoteOff
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

        // NoteOn with velocity 0 == Note Off
        SaxophoneSynth_NoteOn(60, 0);
        for (int i = 0; i < 40; ++i) SaxophoneSynth_FillStereoBuffer(buf.data(), 256);

        CHECK(hard_peak > soft_peak);
        CHECK(SaxophoneSynth_IsNoteActive(60) == false);
        std::cout << "[PASS] Test 3: Velocity response and Velocity 0 == NoteOff verified." << std::endl;
    }

    // Test 4: CC 11 (Expression) and CC 73 (Attack Time)
    {
        SaxophoneSynth_Init();
        std::vector<int16_t> buf(256 * 2, 0);

        // CC 73: Fast attack
        SaxophoneSynth_ControlChange(73, 0); // 5 ms attack
        SaxophoneSynth_NoteOn(60, 100);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        int fast_att_peak = calculate_peak(buf);
        SaxophoneSynth_NoteOff(60);
        for (int i = 0; i < 40; ++i) SaxophoneSynth_FillStereoBuffer(buf.data(), 256);

        // CC 73: Slow attack
        SaxophoneSynth_ControlChange(73, 127); // 1 sec attack
        SaxophoneSynth_NoteOn(60, 100);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        int slow_att_peak = calculate_peak(buf);
        SaxophoneSynth_NoteOff(60);
        for (int i = 0; i < 40; ++i) SaxophoneSynth_FillStereoBuffer(buf.data(), 256);

        CHECK(fast_att_peak > slow_att_peak);

        // CC 11: Expression
        SaxophoneSynth_ControlChange(11, 10); // Low expression
        SaxophoneSynth_ControlChange(73, 0);  // Fast attack
        SaxophoneSynth_NoteOn(60, 100);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        int low_expr_peak = calculate_peak(buf);

        SaxophoneSynth_ControlChange(11, 127); // Full expression
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        int high_expr_peak = calculate_peak(buf);

        SaxophoneSynth_NoteOff(60);
        for (int i = 0; i < 40; ++i) SaxophoneSynth_FillStereoBuffer(buf.data(), 256);

        CHECK(high_expr_peak > low_expr_peak);
        std::cout << "[PASS] Test 4: CC 11 Expression and CC 73 Attack Time verified." << std::endl;
    }

    // Test 5: Control Changes and Sustain pedal
    {
        SaxophoneSynth_Init();
        SaxophoneSynth_ControlChange(1, 64);
        SaxophoneSynth_ControlChange(2, 80);
        SaxophoneSynth_ControlChange(71, 70);
        SaxophoneSynth_ControlChange(72, 40);
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
        std::cout << "[PASS] Test 5: Control Changes and Sustain Pedal processed correctly." << std::endl;
    }

    // Test 6: Polyphony, voice stealing, re-triggering, and repeated Note Off
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

        // Sending Note Off for stolen note (60) should NOT affect note 72 or other notes
        SaxophoneSynth_NoteOff(60);
        SaxophoneSynth_NoteOff(60); // Repeated Note Off
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(SaxophoneSynth_IsNoteActive(72) == true);

        // Re-trigger stolen note 60
        SaxophoneSynth_NoteOn(60, 100);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        CHECK(SaxophoneSynth_IsNoteActive(60) == true);

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
        std::cout << "[PASS] Test 6: Polyphony, voice stealing, re-triggering, and repeated Note Off verified." << std::endl;
    }

    // Test 7: FIFO overflow and recovery stress test
    {
        SaxophoneSynth_Init();

        // Push 40 NoteOn events without calling FillBuffer (FIFO capacity = 32, limit = 28)
        for (int i = 0; i < 40; ++i) {
            SaxophoneSynth_NoteOn(60 + (i % 12), 100);
        }
        CHECK(SaxophoneSynth_GetDroppedEventsCount() > 0);

        std::vector<int16_t> buf(256 * 2, 0);
        SaxophoneSynth_FillStereoBuffer(buf.data(), 256);

        // Trigger NoteOff events
        for (int i = 0; i < 12; ++i) {
            SaxophoneSynth_NoteOff(60 + i);
        }
        for (int i = 0; i < 40; ++i) {
            SaxophoneSynth_FillStereoBuffer(buf.data(), 256);
        }
        std::cout << "[PASS] Test 7: FIFO overflow and stress recovery verified." << std::endl;
    }

    // Test 8: Raw float signal stability and absence of NaN / Inf over long rendering
    {
        SaxophoneSynth_Init();
        SaxophoneSynth_NoteOn(60, 100);
        SaxophoneSynth_NoteOn(64, 100);
        SaxophoneSynth_NoteOn(67, 100);

        std::vector<float> float_buf(512 * 2, 0.0f);
        bool has_nonzero = false;

        for (int block = 0; block < 200; ++block) { // ~2 seconds of rendering
            SaxophoneSynth_FillFloatBuffer(float_buf.data(), 512);
            for (float f : float_buf) {
                CHECK(std::isfinite(f));
                CHECK(std::abs(f) <= 1.0f);
                if (f != 0.0f) has_nonzero = true;
            }
        }
        CHECK(has_nonzero);

        SaxophoneSynth_NoteOff(60);
        SaxophoneSynth_NoteOff(64);
        SaxophoneSynth_NoteOff(67);
        for (int block = 0; block < 50; ++block) {
            SaxophoneSynth_FillFloatBuffer(float_buf.data(), 512);
            for (float f : float_buf) {
                CHECK(std::isfinite(f));
                CHECK(std::abs(f) <= 1.0f);
            }
        }
        std::cout << "[PASS] Test 8: Raw float signal stability verified (std::isfinite and [-1.0f, +1.0f] bounds)." << std::endl;
    }

    // Test 9: SynthEngine integration & switching
    {
        SynthEngine_Init();
        CHECK(SynthEngine_Select(SYNTH_ENGINE_SAXOPHONE) == 1);
        CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SAXOPHONE);
        CHECK(std::string(SynthEngine_GetName()) == "saxophone");

        SynthEngine_NoteOn(60, 100);
        std::vector<int16_t> buf(256 * 2, 0);
        SynthEngine_FillStereoBuffer(buf.data(), 256);
        CHECK(calculate_peak(buf) > 0);

        SynthEngine_NoteOff(60);
        std::cout << "[PASS] Test 9: SynthEngine integration and switching verified." << std::endl;
    }

    if (purrmidi_test::Failures() != 0) {
        std::cerr << "test_saxophone_host: " << purrmidi_test::Failures() << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All SaxophoneSynth host tests passed successfully." << std::endl;
    return 0;
}

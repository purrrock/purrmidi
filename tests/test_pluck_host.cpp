#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include "pluck_synth.h"

static double estimate_fundamental(const int16_t* samples, size_t num_frames, size_t start_offset, size_t window_size, double sample_rate) {
    if (start_offset + window_size + 250 > num_frames) {
        return 0.0;
    }
    double max_r = -1e18;
    size_t best_lag = 0;

    for (size_t lag = 80; lag <= 140; ++lag) {
        double r = 0.0;
        for (size_t i = 0; i < window_size; ++i) {
            double s1 = samples[(start_offset + i) * 2];
            double s2 = samples[(start_offset + i + lag) * 2];
            r += s1 * s2;
        }
        if (r > max_r) {
            max_r = r;
            best_lag = lag;
        }
    }

    if (best_lag == 0) return 0.0;
    return sample_rate / (double)best_lag;
}

void test_pluck_render_and_freq() {
    PluckSynth_Init();

    // NoteOn(69, 100) -> 440 Hz
    PluckSynth_NoteOn(69, 100);

    // 0.5s = 24000 frames
    uint32_t frames_05s = 24000;
    std::vector<int16_t> buf(frames_05s * 2, 0);
    PluckSynth_FillStereoBuffer(buf.data(), frames_05s);

    bool non_silent = false;
    for (size_t i = 0; i < frames_05s; ++i) {
        int16_t l = buf[i * 2];
        int16_t r = buf[i * 2 + 1];
        assert(l == r);
        if (std::abs(l) > 100) {
            non_silent = true;
        }
    }
    assert(non_silent);

    double freq = estimate_fundamental(buf.data(), frames_05s, 4800, 2048, 48000.0);
    std::cout << "Estimated fundamental frequency: " << freq << " Hz (expected ~440 Hz)\n";
    assert(std::abs(freq - 440.0) / 440.0 <= 0.05);

    // NoteOff then render 3s and assert tail is all zeros
    PluckSynth_NoteOff(69);
    uint32_t frames_3s = 48000 * 3;
    std::vector<int16_t> tail_buf(frames_3s * 2, 0);
    PluckSynth_FillStereoBuffer(tail_buf.data(), frames_3s);

    size_t end_start = static_cast<size_t>(48000 * 2.8);
    for (size_t i = end_start; i < frames_3s; ++i) {
        assert(tail_buf[i * 2] == 0);
        assert(tail_buf[i * 2 + 1] == 0);
    }
}

void test_noteon_velocity_zero_regression() {
    PluckSynth_Init();
    PluckSynth_NoteOn(60, 100);

    std::vector<int16_t> buf(48000 * 3 * 2, 0);
    PluckSynth_FillStereoBuffer(buf.data(), 2400); // 0.05s render

    // NoteOn with velocity 0 must behave as NoteOff
    PluckSynth_NoteOn(60, 0);
    PluckSynth_FillStereoBuffer(buf.data(), 48000 * 2.5); // 2.5s render for full damper decay

    assert(PluckSynth_IsNoteActive(60) == false);
    std::cout << "Verified: NoteOn with velocity 0 behaves identically to NoteOff.\n";
}

void test_fifo_overflow_and_press_count_boundary() {
    PluckSynth_Init();

    // 1. Push 25 NoteOn events to FIFO (size = 16) without rendering in between.
    // FIFO capacity is 16; 9 events will be dropped.
    for (int i = 0; i < 25; ++i) {
        PluckSynth_NoteOn(60, 100);
    }

    std::vector<int16_t> buf(48000 * 3 * 2, 0);
    PluckSynth_FillStereoBuffer(buf.data(), 2400); // Render queued FIFO events

    assert(PluckSynth_IsNoteActive(60) == true);

    // Send exactly 16 NoteOff events (matching accepted events)
    for (int i = 0; i < 16; ++i) {
        PluckSynth_NoteOff(60);
    }

    PluckSynth_FillStereoBuffer(buf.data(), 48000 * 2.5); // Render damper decay

    // Verify note 60 is completely damped and not stuck
    assert(PluckSynth_IsNoteActive(60) == false);
    std::cout << "Verified: FIFO overflow drops extra events cleanly without press count corruption.\n";

    // 2. Test saturation boundary (>255 NoteOn events)
    PluckSynth_Init();
    for (int i = 0; i < 300; ++i) {
        PluckSynth_NoteOn(60, 100);
        PluckSynth_FillStereoBuffer(buf.data(), 10); // Process event
    }

    assert(PluckSynth_IsNoteActive(60) == true);

    for (int i = 0; i < 300; ++i) {
        PluckSynth_NoteOff(60);
    }

    PluckSynth_FillStereoBuffer(buf.data(), 48000 * 2.5);
    assert(PluckSynth_IsNoteActive(60) == false);
    std::cout << "Verified: uint8 press count saturation boundary handled cleanly.\n";
}

void test_overlapping_same_pitch_notes() {
    PluckSynth_Init();

    // Press 1 for Note 60
    PluckSynth_NoteOn(60, 100);
    std::vector<int16_t> buf(48000 * 3 * 2, 0);
    PluckSynth_FillStereoBuffer(buf.data(), 2400);

    // Press 2 for Note 60 (overlapping press)
    PluckSynth_NoteOn(60, 100);
    PluckSynth_FillStereoBuffer(buf.data(), 2400);

    // Release Press 1 (NoteOff 60)
    PluckSynth_NoteOff(60);
    PluckSynth_FillStereoBuffer(buf.data(), 4800);

    // Note 60 MUST remain held because Press 2 is still active!
    assert(PluckSynth_IsNoteActive(60) == true);
    std::cout << "Verified: Overlapping Note On holds note when first Note Off arrives.\n";

    // Release Press 2 (NoteOff 60)
    PluckSynth_NoteOff(60);
    PluckSynth_FillStereoBuffer(buf.data(), 48000 * 2.5); // 2.5s render for full damper decay

    // Now Note 60 is fully released and damped
    assert(PluckSynth_IsNoteActive(60) == false);
    std::cout << "Verified: Second Note Off releases note completely.\n";
}

void test_sustain_pedal_and_note_off_isolation() {
    // 1. Note Off BEFORE sustain pedal press: pressing pedal later does NOT resurrect or sustain note
    PluckSynth_Init();
    PluckSynth_ControlChange(64, 0); // Sustain OFF
    PluckSynth_NoteOn(60, 100);

    std::vector<int16_t> buf(48000 * 3 * 2, 0);
    PluckSynth_FillStereoBuffer(buf.data(), 2400); // 0.05s key press

    PluckSynth_NoteOff(60); // NoteOff sent while pedal is OFF
    PluckSynth_FillStereoBuffer(buf.data(), 4800); // 0.1s damper decay

    // Press pedal LATER
    PluckSynth_ControlChange(64, 127);
    std::vector<int16_t> long_buf(48000 * 3 * 2, 0);
    PluckSynth_FillStereoBuffer(long_buf.data(), 48000 * 2.5); // 2.5s render for full damper decay

    assert(PluckSynth_IsNoteActive(60) == false);
    std::cout << "Verified: Note Off before pedal press is not latched by late pedal press.\n";

    // 2. Note Off DURING sustain pedal press: note is held (sustain_held = true)
    PluckSynth_Init();
    PluckSynth_ControlChange(64, 127); // Sustain ON
    PluckSynth_NoteOn(60, 100);

    PluckSynth_FillStereoBuffer(buf.data(), 2400);
    PluckSynth_NoteOff(60); // NoteOff sent while pedal is ON

    PluckSynth_FillStereoBuffer(buf.data(), 24000); // 0.5s render
    assert(PluckSynth_IsNoteActive(60) == true);    // Sustained by CC64
    std::cout << "Verified: Note Off during pedal press latches sustain_held.\n";

    // 3. Releasing pedal (CC64 = 0) initiates damper decay on sustained notes
    PluckSynth_ControlChange(64, 0); // Pedal released!
    PluckSynth_FillStereoBuffer(buf.data(), 48000 * 2.5); // 2.5s render for full damper decay

    assert(PluckSynth_IsNoteActive(60) == false);   // Voice released after damper decay
    std::cout << "Verified: Releasing pedal clears sustain_held and damps note.\n";

    // 4. Note Off of 1 note in a chord does NOT cut or affect the other notes
    PluckSynth_Init();
    PluckSynth_NoteOn(60, 100);
    PluckSynth_NoteOn(64, 100);
    PluckSynth_NoteOn(67, 100);
    PluckSynth_FillStereoBuffer(buf.data(), 2400);

    PluckSynth_NoteOff(60); // NoteOff only for C4
    PluckSynth_FillStereoBuffer(buf.data(), 2400);

    assert(PluckSynth_IsNoteActive(64) == true);
    assert(PluckSynth_IsNoteActive(67) == true);
}

void test_reinit_and_control_regressions() {
    // 1. Re-initialization (PluckSynth_Init) clears active sound and resets sustain
    PluckSynth_Init();
    PluckSynth_ControlChange(64, 127); // sustain on
    PluckSynth_NoteOn(60, 100);

    std::vector<int16_t> buf(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(buf.data(), 4800);

    // Reinit must clear active note & sustain pedal
    PluckSynth_Init();

    std::vector<int16_t> quiet_buf(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(quiet_buf.data(), 4800);
    for (int16_t s : quiet_buf) {
        assert(s == 0);
    }

    // 2. Control changes for Damp (CC1) and Decay (CC72) update synth parameters without crashing
    PluckSynth_ControlChange(1, 100);  // Damp
    PluckSynth_ControlChange(72, 100); // Decay
    PluckSynth_NoteOn(64, 100);
    PluckSynth_FillStereoBuffer(buf.data(), 4800);

    bool non_zero = false;
    for (int16_t s : buf) {
        if (s != 0) { non_zero = true; break; }
    }
    assert(non_zero);
}

void test_deterministic_voice_stealing_and_reuse() {
    PluckSynth_Init();

    // Trigger 8 distinct notes (60..67) sequentially
    for (uint8_t note = 60; note <= 67; ++note) {
        PluckSynth_NoteOn(note, 100);
        std::vector<int16_t> step_buf(100 * 2, 0);
        PluckSynth_FillStereoBuffer(step_buf.data(), 100);
    }

    // Verify all 8 notes 60..67 are currently active
    for (uint8_t note = 60; note <= 67; ++note) {
        assert(PluckSynth_IsNoteActive(note) == true);
    }

    // Note 60 was triggered first (oldest). Trigger 9th note (68).
    PluckSynth_NoteOn(68, 100);
    std::vector<int16_t> steal_buf(100 * 2, 0);
    PluckSynth_FillStereoBuffer(steal_buf.data(), 100);

    // PROOF OF DETERMINISTIC VOICE STEALING:
    // 1. Note 60 (the oldest active note) was stolen!
    assert(PluckSynth_IsNoteActive(60) == false);
    // 2. Note 68 (the new note) took the stolen voice!
    assert(PluckSynth_IsNoteActive(68) == true);
    // 3. Notes 61..67 were NOT stolen and remain active!
    for (uint8_t note = 61; note <= 67; ++note) {
        assert(PluckSynth_IsNoteActive(note) == true);
    }
    std::cout << "Verified deterministic stealing: oldest note 60 stolen, 61..67 remain active, 68 added.\n";

    // Let all notes decay completely and verify voice slots are freed and reused
    PluckSynth_NoteOff(68);
    for (uint8_t note = 61; note <= 67; ++note) {
        PluckSynth_NoteOff(note);
    }

    std::vector<int16_t> decay_buf(48000 * 3 * 2, 0);
    PluckSynth_FillStereoBuffer(decay_buf.data(), 48000 * 3);

    // Verify all voices freed after complete decay
    for (uint8_t note = 60; note <= 68; ++note) {
        assert(PluckSynth_IsNoteActive(note) == false);
    }

    // Trigger new note 72 and verify voice re-allocation
    PluckSynth_NoteOn(72, 100);
    std::vector<int16_t> reuse_buf(1000 * 2, 0);
    PluckSynth_FillStereoBuffer(reuse_buf.data(), 1000);

    assert(PluckSynth_IsNoteActive(72) == true);
}

void test_polyphony_chords_and_clipping() {
    PluckSynth_Init();

    // 1. Single note gain & dynamics test
    PluckSynth_NoteOn(60, 127);
    std::vector<int16_t> single_buf(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(single_buf.data(), 4800);

    int single_peak = 0;
    for (size_t i = 0; i < 4800; ++i) {
        int s = std::abs(single_buf[i * 2]);
        if (s > single_peak) single_peak = s;
    }
    std::cout << "Single note max peak: " << single_peak << "\n";
    assert(single_peak > 20000); // Preserves full single-note dynamic volume

    // 2. 8-note full velocity chord anti-clipping test
    PluckSynth_Init();
    for (uint8_t note = 60; note <= 67; ++note) {
        PluckSynth_NoteOn(note, 127); // Max velocity
    }

    std::vector<int16_t> chord_max_buf(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(chord_max_buf.data(), 4800);

    int hard_clipped_samples = 0;
    int max_chord_peak = 0;
    for (size_t i = 0; i < 4800; ++i) {
        int s = std::abs(chord_max_buf[i * 2]);
        if (s > max_chord_peak) {
            max_chord_peak = s;
        }
        if (s >= 32767) {
            hard_clipped_samples++;
        }
    }

    std::cout << "8-note full-velocity chord peak: " << max_chord_peak
              << ", hard clipped samples: " << hard_clipped_samples << " / 4800\n";
    // Assert ZERO hard clipped samples, proving clean headroom without digital saturation
    assert(hard_clipped_samples == 0);

    // 3. Note Off on 1 note in a 3-note chord reduces energy accordingly
    PluckSynth_Init();
    PluckSynth_NoteOn(60, 100);
    PluckSynth_NoteOn(64, 100);
    PluckSynth_NoteOn(67, 100);

    std::vector<int16_t> chord_3_buf(2400 * 2, 0);
    PluckSynth_FillStereoBuffer(chord_3_buf.data(), 2400);

    double energy_3_notes = 0.0;
    for (size_t i = 1200; i < 2400; ++i) {
        double s = chord_3_buf[i * 2];
        energy_3_notes += s * s;
    }

    PluckSynth_NoteOff(60); // Release C4
    std::vector<int16_t> chord_2_buf(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(chord_2_buf.data(), 4800);

    double energy_2_notes = 0.0;
    for (size_t i = 2400; i < 4800; ++i) {
        double s = chord_2_buf[i * 2];
        energy_2_notes += s * s;
    }

    std::cout << "3-note chord energy: " << energy_3_notes
              << ", 2-note remaining energy: " << energy_2_notes << "\n";
    assert(energy_2_notes < 0.85 * energy_3_notes);
    assert(energy_2_notes > 0.15 * energy_3_notes); // Remaining 2 notes continue sounding
}

int main() {
    test_pluck_render_and_freq();
    test_noteon_velocity_zero_regression();
    test_fifo_overflow_and_press_count_boundary();
    test_overlapping_same_pitch_notes();
    test_sustain_pedal_and_note_off_isolation();
    test_reinit_and_control_regressions();
    test_deterministic_voice_stealing_and_reuse();
    test_polyphony_chords_and_clipping();
    std::cout << "test_pluck_host passed successfully." << std::endl;
    return 0;
}

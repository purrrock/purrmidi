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

void test_sustain_pedal_differentiation() {
    // 1. Measure energy AFTER NoteOff with Sustain pedal OFF
    PluckSynth_Init();
    PluckSynth_ControlChange(64, 0); // Sustain OFF
    PluckSynth_NoteOn(60, 100);

    std::vector<int16_t> buf_off_01s(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(buf_off_01s.data(), 4800); // 0.1s key press

    PluckSynth_NoteOff(60);

    std::vector<int16_t> buf_off_tail(24000 * 2, 0); // 0.5s after note off
    PluckSynth_FillStereoBuffer(buf_off_tail.data(), 24000);

    double energy_off = 0.0;
    // Measure energy in window 0.2s..0.5s after Note Off (samples 9600 to 24000)
    for (size_t i = 9600; i < 24000; ++i) {
        double s = buf_off_tail[i * 2];
        energy_off += s * s;
    }

    // 2. Measure energy AFTER NoteOff with Sustain pedal ON
    PluckSynth_Init();
    PluckSynth_ControlChange(64, 127); // Sustain ON
    PluckSynth_NoteOn(60, 100);

    std::vector<int16_t> buf_on_01s(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(buf_on_01s.data(), 4800);

    PluckSynth_NoteOff(60);

    std::vector<int16_t> buf_on_tail(24000 * 2, 0);
    PluckSynth_FillStereoBuffer(buf_on_tail.data(), 24000);

    double energy_on = 0.0;
    for (size_t i = 9600; i < 24000; ++i) {
        double s = buf_on_tail[i * 2];
        energy_on += s * s;
    }

    std::cout << "Sustain OFF tail energy: " << energy_off
              << ", Sustain ON tail energy: " << energy_on << "\n";

    // Assert that Sustain ON holds significantly more energy than Sustain OFF (damper applied)
    assert(energy_on > 3.0 * energy_off);
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

    // Fill all 8 voices sequentially
    for (uint8_t note = 60; note <= 67; ++note) {
        PluckSynth_NoteOn(note, 100);
        std::vector<int16_t> step_buf(100 * 2, 0);
        PluckSynth_FillStereoBuffer(step_buf.data(), 100);
    }

    // Note 60 was triggered first (oldest). Trigger 9th note (68).
    PluckSynth_NoteOn(68, 100);

    std::vector<int16_t> steal_buf(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(steal_buf.data(), 4800);

    bool sound_active = false;
    for (size_t i = 0; i < 4800; ++i) {
        if (std::abs(steal_buf[i * 2]) > 100) {
            sound_active = true;
            break;
        }
    }
    assert(sound_active);

    // Let all notes decay completely and verify voice slots are freed and reused
    PluckSynth_NoteOff(68);
    for (uint8_t note = 61; note <= 67; ++note) {
        PluckSynth_NoteOff(note);
    }

    std::vector<int16_t> decay_buf(48000 * 3 * 2, 0);
    PluckSynth_FillStereoBuffer(decay_buf.data(), 48000 * 3);

    // Verify silence after complete decay
    for (size_t i = static_cast<size_t>(48000 * 2.8); i < 48000 * 3; ++i) {
        assert(decay_buf[i * 2] == 0);
    }

    // Trigger new note 72 and verify voice re-allocation
    PluckSynth_NoteOn(72, 100);
    std::vector<int16_t> reuse_buf(1000 * 2, 0);
    PluckSynth_FillStereoBuffer(reuse_buf.data(), 1000);

    bool reused_voice_active = false;
    for (size_t i = 0; i < 1000; ++i) {
        if (std::abs(reuse_buf[i * 2]) > 500) {
            reused_voice_active = true;
            break;
        }
    }
    assert(reused_voice_active);
}

void test_polyphony_chords_and_clipping() {
    PluckSynth_Init();

    // 1. Play 3 notes simultaneously (Chord: C4=60, E4=64, G4=67)
    PluckSynth_NoteOn(60, 100);
    PluckSynth_NoteOn(64, 100);
    PluckSynth_NoteOn(67, 100);

    uint32_t frames_05s = 24000;
    std::vector<int16_t> chord_buf(frames_05s * 2, 0);
    PluckSynth_FillStereoBuffer(chord_buf.data(), frames_05s);

    bool chord_active = false;
    for (size_t i = 0; i < frames_05s; ++i) {
        if (std::abs(chord_buf[i * 2]) > 1000) {
            chord_active = true;
            break;
        }
    }
    assert(chord_active);

    // 2. NoteOff on 1 note (C4=60) while E4 and G4 remain active
    PluckSynth_NoteOff(60);

    std::vector<int16_t> partial_off_buf(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(partial_off_buf.data(), 4800);

    bool sound_remains = false;
    for (size_t i = 0; i < 4800; ++i) {
        if (std::abs(partial_off_buf[i * 2]) > 50) {
            sound_remains = true;
            break;
        }
    }
    assert(sound_remains);

    // 3. Test 8-note chord headroom & absence of continuous digital clipping saturation
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
    assert(hard_clipped_samples < 240); // < 5% of samples
}

int main() {
    test_pluck_render_and_freq();
    test_sustain_pedal_differentiation();
    test_reinit_and_control_regressions();
    test_deterministic_voice_stealing_and_reuse();
    test_polyphony_chords_and_clipping();
    std::cout << "test_pluck_host passed successfully." << std::endl;
    return 0;
}

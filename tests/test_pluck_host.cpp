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

void test_noteon_vel_0_and_sustain() {
    // NoteOn velocity 0 behaves as NoteOff
    PluckSynth_Init();
    PluckSynth_NoteOn(60, 100);

    std::vector<int16_t> buf(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(buf.data(), 4800);

    PluckSynth_NoteOn(60, 0);

    std::vector<int16_t> tail(48000 * 3 * 2, 0);
    PluckSynth_FillStereoBuffer(tail.data(), 48000 * 3);

    size_t end_start = static_cast<size_t>(48000 * 2.8);
    for (size_t i = end_start; i < 48000 * 3; ++i) {
        assert(tail[i * 2] == 0);
    }

    // CC64 held keeps sound after NoteOff
    PluckSynth_Init();
    PluckSynth_ControlChange(64, 127);
    PluckSynth_NoteOn(60, 100);

    PluckSynth_FillStereoBuffer(buf.data(), 4800);
    PluckSynth_NoteOff(60);

    std::vector<int16_t> sus_buf(48000 * 2, 0);
    PluckSynth_FillStereoBuffer(sus_buf.data(), 48000);

    bool sound_present = false;
    for (size_t i = static_cast<size_t>(48000 * 0.8); i < 48000; ++i) {
        if (std::abs(sus_buf[i * 2]) > 10) {
            sound_present = true;
            break;
        }
    }
    assert(sound_present);
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

void test_polyphony_chords_and_stealing() {
    PluckSynth_Init();

    // 1. Three distinct notes sounding simultaneously (C4=60, E4=64, G4=67)
    PluckSynth_NoteOn(60, 100);
    PluckSynth_NoteOn(64, 100);
    PluckSynth_NoteOn(67, 100);

    uint32_t frames_05s = 24000;
    std::vector<int16_t> chord_buf(frames_05s * 2, 0);
    PluckSynth_FillStereoBuffer(chord_buf.data(), frames_05s);

    bool chord_active = false;
    int max_chord_sample = 0;
    for (size_t i = 0; i < frames_05s; ++i) {
        int sample_abs = std::abs(chord_buf[i * 2]);
        if (sample_abs > 1000) {
            chord_active = true;
        }
        if (sample_abs > max_chord_sample) {
            max_chord_sample = sample_abs;
        }
    }
    assert(chord_active);

    // 2. NoteOff on 1 note (C4=60) does NOT cut the other two notes (E4 & G4)
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

    // 3. Released strings decay naturally via Karplus-Strong and free voices when silent
    PluckSynth_NoteOff(64);
    PluckSynth_NoteOff(67);
    std::vector<int16_t> full_decay_buf(48000 * 3 * 2, 0);
    PluckSynth_FillStereoBuffer(full_decay_buf.data(), 48000 * 3);

    size_t end_offset = static_cast<size_t>(48000 * 2.8);
    for (size_t i = end_offset; i < 48000 * 3; ++i) {
        assert(full_decay_buf[i * 2] == 0);
    }

    // 4. Voice pool exhaustion: 8 voices active, 9th note steals the oldest voice
    PluckSynth_Init();
    for (uint8_t note = 60; note <= 67; ++note) {
        PluckSynth_NoteOn(note, 100);
    }
    // 9th note steals oldest voice (note 60)
    PluckSynth_NoteOn(68, 100);

    std::vector<int16_t> steal_buf(4800 * 2, 0);
    PluckSynth_FillStereoBuffer(steal_buf.data(), 4800);

    bool steal_sound_active = false;
    for (size_t i = 0; i < 4800; ++i) {
        if (std::abs(steal_buf[i * 2]) > 100) {
            steal_sound_active = true;
            break;
        }
    }
    assert(steal_sound_active);

    // 5. Repeated NoteOn/NoteOff, Sustain, and voice re-allocation
    PluckSynth_Init();
    for (int i = 0; i < 20; ++i) {
        PluckSynth_NoteOn(60, 100);
        PluckSynth_FillStereoBuffer(steal_buf.data(), 100);
        PluckSynth_NoteOff(60);
        PluckSynth_FillStereoBuffer(steal_buf.data(), 100);
    }

    // Re-use freed voices after complete decay
    PluckSynth_FillStereoBuffer(full_decay_buf.data(), 48000 * 3);
    PluckSynth_NoteOn(72, 100);
    PluckSynth_FillStereoBuffer(steal_buf.data(), 1000);
    bool reused_voice_active = false;
    for (size_t i = 0; i < 1000; ++i) {
        if (std::abs(steal_buf[i * 2]) > 500) {
            reused_voice_active = true;
            break;
        }
    }
    assert(reused_voice_active);

    // 6. Check dynamic range and absence of hard clipping saturation on 8-note chord
    PluckSynth_Init();
    for (uint8_t note = 60; note <= 67; ++note) {
        PluckSynth_NoteOn(note, 127); // Full velocity chord
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
    std::cout << "8-note full-velocity chord max peak: " << max_chord_peak
              << ", hard clipped samples: " << hard_clipped_samples << " / 4800\n";
    // Ensure headroom scaling prevents continuous digital hard clipping saturation (< 5% of samples)
    assert(hard_clipped_samples < 240);
}

int main() {
    test_pluck_render_and_freq();
    test_noteon_vel_0_and_sustain();
    test_reinit_and_control_regressions();
    test_polyphony_chords_and_stealing();
    std::cout << "test_pluck_host passed successfully." << std::endl;
    return 0;
}

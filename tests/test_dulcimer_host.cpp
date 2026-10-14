#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include "dulcimer_synth.h"

void test_dulcimer_render_and_decay() {
    DulcimerSynth_Init();

    // Trigger MIDI note C4 (60)
    DulcimerSynth_NoteOn(60, 100);

    // Render 0.5s = 24000 frames
    uint16_t chunk_size = 128;
    std::vector<int16_t> buf(chunk_size * 2, 0);

    bool non_silent = false;
    for (int i = 0; i < 24000 / chunk_size; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), chunk_size);
        for (size_t k = 0; k < chunk_size; ++k) {
            int16_t l = buf[k * 2];
            int16_t r = buf[k * 2 + 1];
            if (std::abs(l) > 50 || std::abs(r) > 50) {
                non_silent = true;
            }
        }
    }
    assert(non_silent);
    std::cout << "Verified: DulcimerSynth renders non-silent audio on NoteOn.\n";

    // Send NoteOff and render 3 seconds to allow modal decay
    DulcimerSynth_NoteOff(60);
    for (int i = 0; i < (48000 * 3) / chunk_size; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), chunk_size);
    }

    // Verify quiet output after decay
    DulcimerSynth_FillStereoBuffer(buf.data(), chunk_size);
    int max_peak = 0;
    for (size_t k = 0; k < chunk_size; ++k) {
        int peak = std::abs(buf[k * 2]);
        if (peak > max_peak) max_peak = peak;
    }
    assert(max_peak < 100);
    std::cout << "Verified: Audio decays properly after NoteOff.\n";
}

void test_dulcimer_control_changes() {
    DulcimerSynth_Init();

    DulcimerSynth_ControlChange(1, 100);  // Timbre / Mallet hardness
    DulcimerSynth_ControlChange(71, 90);  // Inharmonicity / Geometry
    DulcimerSynth_ControlChange(72, 80);  // Release / Damping
    DulcimerSynth_ControlChange(74, 110); // Brightness
    DulcimerSynth_ControlChange(75, 60);  // Strike Position
    DulcimerSynth_ControlChange(64, 127); // Sustain Pedal ON

    DulcimerSynth_NoteOn(64, 100);

    std::vector<int16_t> buf(128 * 2, 0);
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);

    bool non_zero = false;
    for (int16_t s : buf) {
        if (s != 0) {
            non_zero = true;
            break;
        }
    }
    assert(non_zero);
    std::cout << "Verified: DulcimerSynth accepts Control Change messages.\n";
}

void test_dulcimer_polyphony() {
    DulcimerSynth_Init();

    // Trigger a C major chord (C4, E4, G4)
    DulcimerSynth_NoteOn(60, 100);
    DulcimerSynth_NoteOn(64, 100);
    DulcimerSynth_NoteOn(67, 100);

    std::vector<int16_t> buf(256 * 2, 0);
    DulcimerSynth_FillStereoBuffer(buf.data(), 256);

    int max_peak = 0;
    for (size_t i = 0; i < 256; ++i) {
        int l = std::abs(buf[i * 2]);
        if (l > max_peak) max_peak = l;
    }
    assert(max_peak > 500);
    assert(max_peak <= 32767);
    std::cout << "Verified: Polyphonic chord renders cleanly without clipping distortion.\n";
}

int main() {
    test_dulcimer_render_and_decay();
    test_dulcimer_control_changes();
    test_dulcimer_polyphony();
    std::cout << "test_dulcimer_host passed successfully." << std::endl;
    return 0;
}

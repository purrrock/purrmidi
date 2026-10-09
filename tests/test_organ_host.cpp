#include <cstdint>
#include <iostream>
#include <vector>

#include "organ_synth.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond    \
                      << std::endl;                                              \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static int render_peak(uint32_t num_frames) {
    std::vector<int16_t> buf(static_cast<size_t>(num_frames) * 2, 0);
    OrganSynth_FillStereoBuffer(buf.data(), num_frames);
    int peak = 0;
    for (int16_t s : buf) {
        int a = s < 0 ? -static_cast<int>(s) : static_cast<int>(s);
        if (a > peak) peak = a;
    }
    return peak;
}

static void test_init_silence() {
    OrganSynth_Init();
    CHECK(render_peak(512) == 0);
}

static void test_note_on_off() {
    OrganSynth_Init();
    OrganSynth_NoteOn(60, 100);
    CHECK(render_peak(2048) > 0);

    OrganSynth_NoteOff(60);
    // Allow envelope release to finish
    render_peak(2048);
    CHECK(render_peak(1024) == 0);
}

static void test_repeated_note_on() {
    OrganSynth_Init();

    // Repeated Note On for same pitch
    OrganSynth_NoteOn(60, 100);
    render_peak(512);
    OrganSynth_NoteOn(60, 120);
    render_peak(512);

    // Single Note Off should clear all instances of pitch 60
    OrganSynth_NoteOff(60);
    render_peak(2048);
    CHECK(render_peak(1024) == 0);
}

static void test_drawbars_and_chorus() {
    OrganSynth_Init();

    // Enable Chorus
    OrganSynth_ControlChange(70, 127);
    OrganSynth_ControlChange(71, 64);
    OrganSynth_ControlChange(72, 64);

    // Adjust Drawbars (CC 20..28)
    for (uint8_t cc = 20; cc <= 28; ++cc) {
        OrganSynth_ControlChange(cc, 100);
    }

    OrganSynth_NoteOn(64, 110);
    CHECK(render_peak(2048) > 0);

    OrganSynth_NoteOff(64);
    render_peak(2048);
    CHECK(render_peak(1024) == 0);
}

static void test_out_of_bounds_notes() {
    OrganSynth_Init();

    // Out of bounds notes (below C1 = 24 or above C6 = 84)
    OrganSynth_NoteOn(10, 100);
    OrganSynth_NoteOn(100, 100);
    CHECK(render_peak(1024) == 0);

    OrganSynth_NoteOff(10);
    OrganSynth_NoteOff(100);
    render_peak(2048);
    CHECK(render_peak(1024) == 0);
}

static void test_all_notes_off() {
    OrganSynth_Init();

    OrganSynth_NoteOn(60, 100);
    OrganSynth_NoteOn(64, 100);
    OrganSynth_NoteOn(67, 100);
    CHECK(render_peak(2048) > 0);

    // CC 123 = All Notes Off
    OrganSynth_ControlChange(123, 0);
    render_peak(2048);
    CHECK(render_peak(1024) == 0);
}

static void test_queue_overflow_recovery() {
    OrganSynth_Init();

    // Fill queue to overflow (> 64 items)
    for (int i = 0; i < 80; ++i) {
        OrganSynth_NoteOn(static_cast<uint8_t>(30 + (i % 40)), 100);
    }
    CHECK(OrganSynth_GetDroppedEventCount() > 0);

    // Process buffer: overflow panic triggers reset_all()
    CHECK(render_peak(1024) == 0);
}

static void test_init_clears_active_notes_immediately() {
    OrganSynth_Init();

    // Modify envelope attack/release via CC 77/78
    OrganSynth_ControlChange(77, 1);
    OrganSynth_ControlChange(78, 1);

    // Play active notes
    OrganSynth_NoteOn(60, 100);
    OrganSynth_NoteOn(64, 100);
    CHECK(render_peak(256) > 0);

    // Calling OrganSynth_Init must reset all voices and envelope parameters immediately
    OrganSynth_Init();
    CHECK(render_peak(256) == 0);

    // Verify envelope speeds are restored to defaults on subsequent note on
    OrganSynth_NoteOn(60, 100);
    CHECK(render_peak(256) > 0);
    OrganSynth_NoteOff(60);
    render_peak(2048);
    CHECK(render_peak(1024) == 0);
}

static void test_max_drawbars_full_polyphony() {
    OrganSynth_Init();

    // Set all 9 drawbars to max (127)
    for (uint8_t cc = 20; cc <= 28; ++cc) {
        OrganSynth_ControlChange(cc, 127);
    }

    // Play 12 distinct notes simultaneously (full 12-voice polyphony)
    for (uint8_t note = 36; note < 48; ++note) {
        OrganSynth_NoteOn(note, 127);
    }

    std::vector<int16_t> buf(2048 * 2, 0);
    OrganSynth_FillStereoBuffer(buf.data(), 2048);

    int peak = 0;
    for (int16_t sample : buf) {
        int abs_val = sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
        if (abs_val > peak) peak = abs_val;
    }

    CHECK(peak > 0);
    CHECK(peak <= 32767);

    // Clean release via All Notes Off
    OrganSynth_ControlChange(123, 0);
    render_peak(2048);
    CHECK(render_peak(1024) == 0);
}

int main() {
    test_init_silence();
    test_note_on_off();
    test_repeated_note_on();
    test_drawbars_and_chorus();
    test_out_of_bounds_notes();
    test_all_notes_off();
    test_queue_overflow_recovery();
    test_init_clears_active_notes_immediately();
    test_max_drawbars_full_polyphony();

    if (g_failures != 0) {
        std::cerr << "test_organ_host: " << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_organ_host passed successfully." << std::endl;
    return 0;
}

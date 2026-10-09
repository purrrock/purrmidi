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

int main() {
    test_init_silence();
    test_note_on_off();
    test_drawbars_and_chorus();
    test_out_of_bounds_notes();
    test_all_notes_off();

    if (g_failures != 0) {
        std::cerr << "test_organ_host: " << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_organ_host passed successfully." << std::endl;
    return 0;
}

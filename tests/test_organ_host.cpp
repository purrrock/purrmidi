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

static std::vector<int16_t> render_mono(uint32_t num_frames) {
    std::vector<int16_t> buf(static_cast<size_t>(num_frames) * 2, 0);
    OrganSynth_FillStereoBuffer(buf.data(), num_frames);
    std::vector<int16_t> mono(num_frames);
    for (uint32_t i = 0; i < num_frames; ++i) mono[i] = buf[i * 2];
    return mono;
}

static int peak_of(const std::vector<int16_t>& v, size_t from = 0) {
    int peak = 0;
    for (size_t i = from; i < v.size(); ++i) {
        int a = v[i] < 0 ? -static_cast<int>(v[i]) : static_cast<int>(v[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

// Largest |s[i]-s[i-1]| including the step from `prev` to the first sample.
static int max_step(int prev, const std::vector<int16_t>& v, size_t from = 0) {
    int m = 0;
    for (size_t i = from; i < v.size(); ++i) {
        int d = static_cast<int>(v[i]) - prev;
        if (d < 0) d = -d;
        if (d > m) m = d;
        prev = v[i];
    }
    return m;
}

// With the x4 output gain a slow attack (CC77=1) reaches ~12% of full level after 512 frames.
// A full-level single note peaks at ~2900, so anything below this limit is "still ramping".
static const int kSlowAttackPeakLimit = 800;

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

static void test_cc77_cc78_preserved_after_cc120() {
    OrganSynth_Init();

    // Set slow attack (CC 77) and slow release (CC 78)
    OrganSynth_ControlChange(77, 1);
    OrganSynth_ControlChange(78, 1);

    OrganSynth_NoteOn(60, 100);
    render_peak(1024);

    // CC 120 = All Sound Off
    OrganSynth_ControlChange(120, 0);
    CHECK(render_peak(128) == 0);

    // Play new note and verify slow attack is preserved
    OrganSynth_NoteOn(60, 100);
    int peak_slow_attack = render_peak(512);
    CHECK(peak_slow_attack < kSlowAttackPeakLimit);

    render_peak(1024); // reach sustain
    OrganSynth_NoteOff(60);

    // Verify slow release is preserved (sound is still playing after 300 frames)
    int peak_slow_release = render_peak(300);
    CHECK(peak_slow_release > 0);
}

static void test_cc77_cc78_preserved_after_cc123() {
    OrganSynth_Init();

    // Set slow attack (CC 77) and slow release (CC 78)
    OrganSynth_ControlChange(77, 1);
    OrganSynth_ControlChange(78, 1);

    OrganSynth_NoteOn(60, 100);
    render_peak(1024);

    // CC 123 = All Notes Off: voices fade out through their (slow) release, no hard cut
    OrganSynth_ControlChange(123, 0);
    CHECK(render_peak(128) > 0);
    render_peak(8192);
    CHECK(render_peak(128) == 0);

    // Play new note and verify slow attack is preserved
    OrganSynth_NoteOn(60, 100);
    int peak_slow_attack = render_peak(512);
    CHECK(peak_slow_attack < kSlowAttackPeakLimit);

    render_peak(1024); // reach sustain
    OrganSynth_NoteOff(60);

    // Verify slow release is preserved
    int peak_slow_release = render_peak(300);
    CHECK(peak_slow_release > 0);
}

static void test_cc77_cc78_preserved_after_fifo_overflow() {
    OrganSynth_Init();

    // Set slow attack (CC 77) and slow release (CC 78) and process them
    OrganSynth_ControlChange(77, 1);
    OrganSynth_ControlChange(78, 1);
    render_peak(128); // process CC events into synth state

    // Fill queue to overflow (> 64 items)
    for (int i = 0; i < 80; ++i) {
        OrganSynth_NoteOn(static_cast<uint8_t>(30 + (i % 40)), 100);
    }
    CHECK(OrganSynth_GetDroppedEventCount() > 0);

    // Overflow recovery flushes FIFO and resets all voices
    CHECK(render_peak(128) == 0);

    // Play new note and verify slow attack is preserved
    OrganSynth_NoteOn(60, 100);
    int peak_slow_attack = render_peak(512);
    CHECK(peak_slow_attack < kSlowAttackPeakLimit);

    render_peak(1024); // reach sustain
    OrganSynth_NoteOff(60);

    // Verify slow release is preserved
    int peak_slow_release = render_peak(300);
    CHECK(peak_slow_release > 0);
}

static void test_cc77_cc78_reset_on_reinit() {
    OrganSynth_Init();

    // Set slow attack (CC 77) and slow release (CC 78)
    OrganSynth_ControlChange(77, 1);
    OrganSynth_ControlChange(78, 1);

    // Play active notes
    OrganSynth_NoteOn(60, 100);
    OrganSynth_NoteOn(64, 100);
    CHECK(render_peak(256) > 0);

    // Calling OrganSynth_Init must reset all voices and restore envelope parameters to defaults
    OrganSynth_Init();
    CHECK(render_peak(256) == 0);

    // Verify envelope attack speed is restored to default (fast attack)
    OrganSynth_NoteOn(60, 100);
    int peak_fast_attack = render_peak(512);
    CHECK(peak_fast_attack > 500);

    render_peak(1024); // reach sustain
    OrganSynth_NoteOff(60);

    // Verify envelope release speed is restored to default (sound dies off within 500 frames)
    render_peak(500);
    CHECK(render_peak(256) == 0);
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

// 1. Output level: upstream shift (>>20) gave ~-33 dBFS for one note.
static void test_output_level() {
    OrganSynth_Init();
    OrganSynth_NoteOn(60, 100);
    render_peak(2000);
    int single = render_peak(4800);
    CHECK(single > 1500);
    CHECK(single <= 32767);

    OrganSynth_Init();
    for (uint8_t cc = 20; cc <= 28; ++cc) OrganSynth_ControlChange(cc, 127);
    for (uint8_t n = 48; n < 60; ++n) OrganSynth_NoteOn(n, 100);
    render_peak(2000);
    int chord = render_peak(4800);
    CHECK(chord > 8000);
    CHECK(chord <= 32767);
}

// 2. CC120 cuts immediately, CC123 releases without a click.
static void test_all_sound_off_vs_all_notes_off() {
    OrganSynth_Init();
    OrganSynth_NoteOn(60, 100);
    render_peak(2000);
    OrganSynth_ControlChange(120, 0);
    CHECK(render_peak(64) == 0);

    OrganSynth_Init();
    OrganSynth_NoteOn(60, 100);
    std::vector<int16_t> steady = render_mono(4000);
    int ref_step = max_step(0, steady, 2000);
    OrganSynth_ControlChange(123, 0);
    std::vector<int16_t> tail = render_mono(600);
    CHECK(peak_of(tail) > 0);                                   // not cut abruptly
    CHECK(max_step(steady.back(), tail) * 2 <= ref_step * 3);   // no step above 1.5x normal
    render_peak(2048);
    CHECK(render_peak(512) == 0);                               // but it does end
}

// 3. FIFO overflow must keep queued controller changes (only notes are dropped).
static void test_overflow_keeps_controllers() {
    OrganSynth_Init();
    for (uint8_t cc = 20; cc <= 28; ++cc) OrganSynth_ControlChange(cc, 0);   // all drawbars in
    for (int i = 0; i < 70; ++i) {
        OrganSynth_NoteOn(static_cast<uint8_t>(30 + (i % 40)), 100);
    }
    CHECK(OrganSynth_GetDroppedEventCount() > 0);
    CHECK(render_peak(256) == 0);

    OrganSynth_NoteOn(60, 100);
    CHECK(render_peak(2048) == 0);                              // drawbars still at 0

    OrganSynth_ControlChange(22, 127);                          // bring in 8'
    render_peak(256);
    CHECK(render_peak(2048) > 0);
}

// 4a. With all voices busy the quietest *releasing* voice is stolen first.
static void test_steal_prefers_releasing_voice() {
    OrganSynth_Init();
    OrganSynth_ControlChange(78, 2);                            // slow release
    for (uint8_t n = 40; n < 52; ++n) OrganSynth_NoteOn(n, 100);
    render_peak(4096);
    OrganSynth_NoteOff(51);
    render_peak(100);
    OrganSynth_NoteOn(70, 100);                                 // 13th note
    render_peak(100);

    for (uint8_t n = 41; n < 51; ++n) OrganSynth_NoteOff(n);
    OrganSynth_NoteOff(70);
    render_peak(16384);
    CHECK(render_peak(512) > 0);                                // note 40 must still be held

    OrganSynth_NoteOff(40);
    render_peak(16384);
    CHECK(render_peak(512) == 0);
}

// 4b. With no releasing voice, the voice triggered longest ago is stolen (not always voice 0).
static void test_steal_oldest_voice() {
    OrganSynth_Init();
    for (uint8_t n = 40; n < 52; ++n) OrganSynth_NoteOn(n, 100);
    render_peak(4096);
    OrganSynth_NoteOn(40, 100);                                 // re-trigger: 40 is now the newest
    render_peak(256);
    OrganSynth_NoteOn(70, 100);                                 // steals the oldest = note 41
    render_peak(256);

    for (uint8_t n = 42; n < 52; ++n) OrganSynth_NoteOff(n);
    OrganSynth_NoteOff(40);
    OrganSynth_NoteOff(70);
    render_peak(8192);
    CHECK(render_peak(512) == 0);                               // 41 was stolen: nothing left
}

// 5. Re-triggering a note that is still releasing must continue from the current level.
static void test_retrigger_in_release_has_no_dip() {
    OrganSynth_Init();
    OrganSynth_ControlChange(77, 2);                            // slow attack (~2048 frames)
    OrganSynth_ControlChange(78, 2);                            // slow release
    OrganSynth_NoteOn(60, 100);
    render_peak(4096);                                          // reach sustain
    OrganSynth_NoteOff(60);
    std::vector<int16_t> before = render_mono(1000);            // roughly half way down
    int peak_before = peak_of(before, 744);                     // last 256 frames
    CHECK(peak_before > 0);

    OrganSynth_NoteOn(60, 100);
    int peak_after = render_peak(256);
    CHECK(peak_after * 2 >= peak_before);                       // no restart from zero
}

int main() {
    test_init_silence();
    test_note_on_off();
    test_repeated_note_on();
    test_drawbars_and_chorus();
    test_out_of_bounds_notes();
    test_all_notes_off();
    test_queue_overflow_recovery();
    test_cc77_cc78_preserved_after_cc120();
    test_cc77_cc78_preserved_after_cc123();
    test_cc77_cc78_preserved_after_fifo_overflow();
    test_cc77_cc78_reset_on_reinit();
    test_max_drawbars_full_polyphony();
    test_output_level();
    test_all_sound_off_vs_all_notes_off();
    test_overflow_keeps_controllers();
    test_steal_prefers_releasing_voice();
    test_steal_oldest_voice();
    test_retrigger_in_release_has_no_dip();

    if (g_failures != 0) {
        std::cerr << "test_organ_host: " << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_organ_host passed successfully." << std::endl;
    return 0;
}

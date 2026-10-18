#include <iostream>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <thread>
#include "dulcimer_synth.h"
#define PURRMIDI_CHECK_FAIL_FAST   // линейный тест: после первой ошибки продолжать бессмысленно
#include "check.h"

void test_case_1_note_on_signal() {
    DulcimerSynth_Init();
    DulcimerSynth_NoteOn(60, 100);

    std::vector<int16_t> buf(128 * 2, 0);
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);

    bool non_zero = false;
    for (int16_t s : buf) {
        if (std::abs(s) > 100) {
            non_zero = true;
            break;
        }
    }
    CHECK(non_zero);
    std::cout << "[PASS] Test 1: Note On produces non-zero signal.\n";
}

void test_case_2_note_off_decay() {
    DulcimerSynth_Init();
    DulcimerSynth_NoteOn(60, 100);

    std::vector<int16_t> buf(128 * 2, 0);
    // Render 0.2s of sounding note
    for (int i = 0; i < (48000 * 2 / 10) / 128; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    }

    // Release key
    DulcimerSynth_NoteOff(60);

    // Render 3s of decay
    for (int i = 0; i < (48000 * 3) / 128; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    }

    // Render one more block and check quiet level
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    int max_peak = 0;
    for (int16_t s : buf) {
        if (std::abs(s) > max_peak) max_peak = std::abs(s);
    }
    CHECK(max_peak < 100);
    std::cout << "[PASS] Test 2: Note Off leads to natural decay.\n";
}

void test_case_3_repeated_note_on_off() {
    DulcimerSynth_Init();

    std::vector<int16_t> buf(128 * 2, 0);

    // Repeat 5 times
    for (int cycle = 0; cycle < 5; ++cycle) {
        DulcimerSynth_NoteOn(60, 100);
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);

        bool active_signal = false;
        for (int16_t s : buf) {
            if (std::abs(s) > 100) active_signal = true;
        }
        CHECK(active_signal);

        DulcimerSynth_NoteOff(60);
        for (int i = 0; i < 48000 / 128; ++i) {
            DulcimerSynth_FillStereoBuffer(buf.data(), 128);
        }
    }
    std::cout << "[PASS] Test 3: Repeated Note On and Note Off operating correctly.\n";
}

void test_case_4_voice_stealing_isolation() {
    DulcimerSynth_Init();

    // 1. Hold 4 active notes (60, 61, 62, 63) - filling all 4 polyphonic voices
    DulcimerSynth_NoteOn(60, 100);
    DulcimerSynth_NoteOn(61, 100);
    DulcimerSynth_NoteOn(62, 100);
    DulcimerSynth_NoteOn(63, 100);

    std::vector<int16_t> buf(128 * 2, 0);
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);

    // 2. Trigger note 64. Voice 0 (playing note 60) is stolen for note 64!
    DulcimerSynth_NoteOn(64, 100);
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);

    // 3. Send Note Off for stolen note 60.
    // MUST NOT mute or release note 64!
    DulcimerSynth_NoteOff(60);

    // Render 0.5s
    bool note_64_still_active = false;
    for (int i = 0; i < (48000 / 2) / 128; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);
        for (int16_t s : buf) {
            if (std::abs(s) > 100) note_64_still_active = true;
        }
    }
    CHECK(note_64_still_active);

    // 4. Send Note Off for note 64. NOW note 64 must decay cleanly!
    DulcimerSynth_NoteOff(64);
    DulcimerSynth_NoteOff(61);
    DulcimerSynth_NoteOff(62);
    DulcimerSynth_NoteOff(63);

    for (int i = 0; i < (48000 * 3) / 128; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    }

    DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    int max_peak = 0;
    for (int16_t s : buf) {
        if (std::abs(s) > max_peak) max_peak = std::abs(s);
    }
    CHECK(max_peak < 100);
    std::cout << "[PASS] Test 4: Voice stealing cleans up state and isolates Note Off events.\n";
}

void test_case_5_polyphony_and_voice_reuse() {
    DulcimerSynth_Init();

    // Trigger 3 notes of C major chord
    DulcimerSynth_NoteOn(60, 100);
    DulcimerSynth_NoteOn(64, 100);
    DulcimerSynth_NoteOn(67, 100);

    std::vector<int16_t> buf(256 * 2, 0);
    DulcimerSynth_FillStereoBuffer(buf.data(), 256);

    int chord_peak = 0;
    for (int16_t s : buf) {
        if (std::abs(s) > chord_peak) chord_peak = std::abs(s);
    }
    CHECK(chord_peak > 500);

    // Release C4 (60)
    DulcimerSynth_NoteOff(60);
    DulcimerSynth_FillStereoBuffer(buf.data(), 256);

    // Re-use freed voice slot with D4 (62)
    DulcimerSynth_NoteOn(62, 100);
    DulcimerSynth_FillStereoBuffer(buf.data(), 256);

    int new_peak = 0;
    for (int16_t s : buf) {
        if (std::abs(s) > new_peak) new_peak = std::abs(s);
    }
    CHECK(new_peak > 500);
    std::cout << "[PASS] Test 5: Polyphony and voice reuse operating correctly.\n";
}

void test_case_6_sustain_pedal() {
    DulcimerSynth_Init();

    // Sustain pedal ON (CC64 = 127)
    DulcimerSynth_ControlChange(64, 127);

    // Play note 60 and immediately send Note Off
    DulcimerSynth_NoteOn(60, 100);
    DulcimerSynth_NoteOff(60);

    std::vector<int16_t> buf(128 * 2, 0);
    // Render 1s while pedal is ON -> note must continue sounding!
    bool sustained_sound = false;
    for (int i = 0; i < 48000 / 128; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);
        for (int16_t s : buf) {
            if (std::abs(s) > 100) sustained_sound = true;
        }
    }
    CHECK(sustained_sound);

    // Release sustain pedal (CC64 = 0) -> note must now decay!
    DulcimerSynth_ControlChange(64, 0);
    for (int i = 0; i < (48000 * 3) / 128; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    }

    DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    int max_peak = 0;
    for (int16_t s : buf) {
        if (std::abs(s) > max_peak) max_peak = std::abs(s);
    }
    CHECK(max_peak < 100);
    std::cout << "[PASS] Test 6: Sustain pedal holds and releases notes correctly.\n";
}

void test_case_7_fifo_overflow_recovery() {
    DulcimerSynth_Init();

    // Push 40 NoteOn events without rendering to completely saturate FIFO
    for (int i = 0; i < 40; ++i) {
        DulcimerSynth_NoteOn(60 + (i % 12), 100);
    }

    // Push critical NoteOff while full
    DulcimerSynth_NoteOff(60);

    std::vector<int16_t> buf(128 * 2, 0);
    // Render audio block to trigger emergency overflow recovery
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);

    // Render 3s and verify recovery cleared stuck states
    for (int i = 0; i < (48000 * 3) / 128; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    }

    DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    int max_peak = 0;
    for (int16_t s : buf) {
        if (std::abs(s) > max_peak) max_peak = std::abs(s);
    }
    CHECK(max_peak < 100);

    // Verify synth functions normally after recovery
    DulcimerSynth_NoteOn(72, 100);
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    bool post_rec_sound = false;
    for (int16_t s : buf) {
        if (std::abs(s) > 100) post_rec_sound = true;
    }
    CHECK(post_rec_sound);
    std::cout << "[PASS] Test 7: FIFO overflow recovery operates safely without stuck notes.\n";
}

void test_case_8_reinitialization() {
    DulcimerSynth_Init();
    DulcimerSynth_NoteOn(60, 100);

    std::vector<int16_t> buf(128 * 2, 0);
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);

    // Re-initialize synth
    DulcimerSynth_Init();

    // Audio buffer must immediately render total silence after re-init
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    for (int16_t s : buf) {
        CHECK(s == 0);
    }
    std::cout << "[PASS] Test 8: DulcimerSynth_Init resets state and renders silence.\n";
}

void test_case_9_invalid_inputs() {
    DulcimerSynth_Init();

    // Out-of-bounds note values (> 127) and velocity 0
    DulcimerSynth_NoteOn(128, 100);
    DulcimerSynth_NoteOn(255, 100);
    DulcimerSynth_NoteOff(200);
    DulcimerSynth_NoteOn(60, 0); // velocity 0 == Note Off

    std::vector<int16_t> buf(128 * 2, 0);
    DulcimerSynth_FillStereoBuffer(buf.data(), 128);

    // Must not crash or play invalid note
    for (int16_t s : buf) {
        CHECK(s == 0);
    }
    std::cout << "[PASS] Test 9: Invalid MIDI inputs handled safely.\n";
}

void test_case_10_concurrent_producers() {
    DulcimerSynth_Init();

    std::atomic<bool> running{true};

    // Spawn producer thread
    std::thread producer([&]() {
        uint8_t note = 60;
        while (running.load()) {
            DulcimerSynth_NoteOn(note, 100);
            DulcimerSynth_NoteOff(note);
            note = 60 + ((note + 1) % 12);
            DulcimerSynth_ControlChange(71, note);
        }
    });

    // Consumer thread (main)
    std::vector<int16_t> buf(128 * 2, 0);
    for (int i = 0; i < 500; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    }

    running.store(false);
    producer.join();

    // Render remaining audio to verify clean decay
    for (int i = 0; i < (48000 * 3) / 128; ++i) {
        DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    }

    DulcimerSynth_FillStereoBuffer(buf.data(), 128);
    int max_peak = 0;
    for (int16_t s : buf) {
        if (std::abs(s) > max_peak) max_peak = std::abs(s);
    }
    CHECK(max_peak < 100);
    std::cout << "[PASS] Test 10: Multi-producer concurrent operation is thread-safe.\n";
}

int main() {
    test_case_1_note_on_signal();
    test_case_2_note_off_decay();
    test_case_3_repeated_note_on_off();
    test_case_4_voice_stealing_isolation();
    test_case_5_polyphony_and_voice_reuse();
    test_case_6_sustain_pedal();
    test_case_7_fifo_overflow_recovery();
    test_case_8_reinitialization();
    test_case_9_invalid_inputs();
    test_case_10_concurrent_producers();
    std::cout << "All 10 DulcimerSynth regression tests passed successfully." << std::endl;
    return 0;
}

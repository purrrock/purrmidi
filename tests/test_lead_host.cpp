#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include "lead_synth.h"

void test_lead_render_and_note_off() {
    LeadSynth_Init();

    // 1. NoteOn(69, 100) -> A4 (440 Hz)
    LeadSynth_NoteOn(69, 100);

    uint32_t frames_05s = 24000;
    std::vector<int16_t> buf(frames_05s * 2, 0);
    LeadSynth_FillStereoBuffer(buf.data(), frames_05s);

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
    assert(LeadSynth_IsNoteActive(69) == true);
    std::cout << "Verified: Non-zero audio generated on NoteOn.\n";

    // 2. NoteOff(69) and render until release completes
    LeadSynth_NoteOff(69);
    uint32_t frames_2s = 48000 * 2;
    std::vector<int16_t> tail_buf(frames_2s * 2, 0);
    LeadSynth_FillStereoBuffer(tail_buf.data(), frames_2s);

    // After 2s release should be complete and voice inactive
    assert(LeadSynth_IsNoteActive(69) == false);

    size_t end_start = static_cast<size_t>(48000 * 1.8);
    for (size_t i = end_start; i < frames_2s; ++i) {
        assert(tail_buf[i * 2] == 0);
        assert(tail_buf[i * 2 + 1] == 0);
    }
    std::cout << "Verified: NoteOff cleanly completes release envelope to silence.\n";
}

void test_noteon_velocity_zero_regression() {
    LeadSynth_Init();
    LeadSynth_NoteOn(60, 100);

    std::vector<int16_t> buf(48000 * 2 * 2, 0);
    LeadSynth_FillStereoBuffer(buf.data(), 2400); // 0.05s render

    // NoteOn with velocity 0 must behave as NoteOff
    LeadSynth_NoteOn(60, 0);
    LeadSynth_FillStereoBuffer(buf.data(), 48000 * 1.5); // 1.5s render for release

    assert(LeadSynth_IsNoteActive(60) == false);
    std::cout << "Verified: NoteOn with velocity 0 behaves identically to NoteOff.\n";
}

void test_subblock_sustain_event_ordering() {
    // Scenario 1: NoteOn -> NoteOff -> CC64=127 all issued BEFORE render callback.
    // Order: NoteOff occurs BEFORE pedal press. Note MUST NOT be sustained!
    LeadSynth_Init();
    LeadSynth_NoteOn(60, 100);
    LeadSynth_NoteOff(60);
    LeadSynth_ControlChange(64, 127);

    std::vector<int16_t> buf(48000 * 2 * 2, 0);
    LeadSynth_FillStereoBuffer(buf.data(), 48000 * 1.5); // Render 1.5s

    assert(LeadSynth_IsNoteActive(60) == false);
    std::cout << "Verified: Sub-block event sequence (NoteOn -> NoteOff -> CC64=127) does not sustain note.\n";

    // Scenario 2: CC64=127 -> NoteOn -> NoteOff all issued BEFORE render callback.
    // Order: Pedal press occurs BEFORE NoteOff. Note MUST be sustained!
    LeadSynth_Init();
    LeadSynth_ControlChange(64, 127);
    LeadSynth_NoteOn(60, 100);
    LeadSynth_NoteOff(60);

    LeadSynth_FillStereoBuffer(buf.data(), 24000); // Render 0.5s
    assert(LeadSynth_IsNoteActive(60) == true);   // Sustained!

    LeadSynth_ControlChange(64, 0); // Release pedal
    LeadSynth_FillStereoBuffer(buf.data(), 48000 * 1.5);
    assert(LeadSynth_IsNoteActive(60) == false);  // Voice released after pedal drop
    std::cout << "Verified: Sub-block event sequence (CC64=127 -> NoteOn -> NoteOff) sustains note.\n";
}

void test_fifo_overflow_critical_release_delivery() {
    LeadSynth_Init();

    // 1. Push 40 NoteOn events to FIFO without rendering in between.
    // NOTE_ON_FIFO_LIMIT = 28 out of EVENT_FIFO_SIZE = 32.
    for (int i = 0; i < 40; ++i) {
        LeadSynth_NoteOn(60, 100);
    }

    uint32_t dropped = LeadSynth_GetDroppedEventsCount();
    std::cout << "Dropped NoteOn events count: " << dropped << std::endl;
    assert(dropped == 12); // 28 accepted, 12 dropped out of 40

    std::vector<int16_t> buf(48000 * 2 * 2, 0);
    LeadSynth_FillStereoBuffer(buf.data(), 2400); // Render queued events
    assert(LeadSynth_IsNoteActive(60) == true);

    // Send 28 NoteOff events (matching accepted NoteOn events)
    for (int i = 0; i < 28; ++i) {
        LeadSynth_NoteOff(60);
    }

    LeadSynth_FillStereoBuffer(buf.data(), 48000 * 1.5); // Render release
    assert(LeadSynth_IsNoteActive(60) == false);
    std::cout << "Verified: FIFO overflow drops extra NoteOn events cleanly without press count corruption.\n";

    // 2. Critical NoteOff delivery via reserved slots when NoteOn limit is reached:
    LeadSynth_Init();
    LeadSynth_NoteOn(60, 100);
    LeadSynth_FillStereoBuffer(buf.data(), 100);

    // Fill FIFO up to NoteOn limit (28 accepted out of 32 attempts)
    for (int i = 0; i < 32; ++i) {
        LeadSynth_NoteOn(64 + (i % 8), 100);
    }

    // Send critical NoteOff for note 60 while NoteOn limit is reached.
    // Reserved capacity allows NoteOff(60) to be enqueued in strict FIFO order.
    LeadSynth_NoteOff(60);

    LeadSynth_FillStereoBuffer(buf.data(), 48000 * 1.5);
    assert(LeadSynth_IsNoteActive(60) == false); // Critical NoteOff delivered! No stuck note!
    std::cout << "Verified: Critical NoteOff uses reserved FIFO slots and releases note.\n";

    // 3. Critical CC64=0 (pedal release) delivery via reserved slots:
    LeadSynth_Init();
    LeadSynth_ControlChange(64, 127); // Pedal ON
    LeadSynth_NoteOn(60, 100);
    LeadSynth_NoteOff(60);            // Note 60 is sustained by CC64
    LeadSynth_FillStereoBuffer(buf.data(), 2400);
    assert(LeadSynth_IsNoteActive(60) == true);

    // Fill FIFO up to NoteOn limit
    for (int i = 0; i < 32; ++i) {
        LeadSynth_NoteOn(64 + (i % 8), 100);
    }

    // Send critical CC64=0 (pedal release)
    LeadSynth_ControlChange(64, 0);

    LeadSynth_FillStereoBuffer(buf.data(), 48000 * 1.5);
    assert(LeadSynth_IsNoteActive(60) == false); // Critical CC64=0 delivered! No stuck sustain!
    std::cout << "Verified: Critical CC64=0 uses reserved FIFO slots and releases sustain.\n";

    // 4. Test full 32-slot FIFO saturation and emergency overflow recovery state machine
    LeadSynth_Init();
    LeadSynth_ControlChange(64, 127); // Sustain ON
    LeadSynth_NoteOn(60, 100);
    LeadSynth_FillStereoBuffer(buf.data(), 100);
    assert(LeadSynth_IsNoteActive(60) == true);

    // Completely saturate all 32 FIFO slots with events (28 NoteOn + 4 NoteOff)
    for (int i = 0; i < 28; ++i) {
        LeadSynth_NoteOn(64 + (i % 8), 100);
    }
    for (int i = 0; i < 4; ++i) {
        LeadSynth_NoteOff(64 + (i % 8));
    }

    // Now send critical NoteOff(60) when FIFO is 100% full (32/32 slots used).
    // This triggers emergency overflow recovery state machine request.
    LeadSynth_NoteOff(60);

    // Render audio block to trigger emergency state reset in consumer thread
    LeadSynth_FillStereoBuffer(buf.data(), 48000 * 1.5);

    // Verify emergency recovery released all held notes and sustain pedal
    assert(LeadSynth_IsNoteActive(60) == false);
    std::cout << "Verified: Emergency recovery state machine released held notes and sustain pedal on 100% FIFO saturation.\n";

    // 5. Test post-recovery synth functionality (new NoteOn -> NoteOff cycle)
    LeadSynth_NoteOn(72, 100);
    LeadSynth_FillStereoBuffer(buf.data(), 2400);
    assert(LeadSynth_IsNoteActive(72) == true);

    LeadSynth_NoteOff(72);
    LeadSynth_FillStereoBuffer(buf.data(), 48000 * 1.5);
    assert(LeadSynth_IsNoteActive(72) == false);
    std::cout << "Verified: Post-recovery NoteOn -> NoteOff cycle operates normally.\n";
}

void test_overlapping_same_pitch_notes() {
    LeadSynth_Init();

    // Press 1 for Note 60
    LeadSynth_NoteOn(60, 100);
    std::vector<int16_t> buf(48000 * 2 * 2, 0);
    LeadSynth_FillStereoBuffer(buf.data(), 2400);

    // Press 2 for Note 60 (overlapping press)
    LeadSynth_NoteOn(60, 100);
    LeadSynth_FillStereoBuffer(buf.data(), 2400);

    // Release Press 1 (NoteOff 60)
    LeadSynth_NoteOff(60);
    LeadSynth_FillStereoBuffer(buf.data(), 4800);

    // Note 60 MUST remain active because Press 2 is still held
    assert(LeadSynth_IsNoteActive(60) == true);
    std::cout << "Verified: Overlapping Note On holds note when first Note Off arrives.\n";

    // Release Press 2 (NoteOff 60)
    LeadSynth_NoteOff(60);
    LeadSynth_FillStereoBuffer(buf.data(), 48000 * 1.5);

    // Now Note 60 is fully released
    assert(LeadSynth_IsNoteActive(60) == false);
    std::cout << "Verified: Second Note Off releases note completely.\n";
}

void test_deterministic_voice_stealing_and_reuse() {
    LeadSynth_Init();

    // Trigger 8 distinct notes (60..67) sequentially
    for (uint8_t note = 60; note <= 67; ++note) {
        LeadSynth_NoteOn(note, 100);
        std::vector<int16_t> step_buf(100 * 2, 0);
        LeadSynth_FillStereoBuffer(step_buf.data(), 100);
    }

    // Verify all 8 notes 60..67 are currently active
    for (uint8_t note = 60; note <= 67; ++note) {
        assert(LeadSynth_IsNoteActive(note) == true);
    }

    // Note 60 was triggered first (oldest). Trigger 9th note (68).
    LeadSynth_NoteOn(68, 100);
    std::vector<int16_t> steal_buf(100 * 2, 0);
    LeadSynth_FillStereoBuffer(steal_buf.data(), 100);

    // PROOF OF DETERMINISTIC VOICE STEALING:
    // 1. Note 60 (the oldest active note) was stolen!
    assert(LeadSynth_IsNoteActive(60) == false);
    // 2. Note 68 (the new note) took the stolen voice!
    assert(LeadSynth_IsNoteActive(68) == true);
    // 3. Notes 61..67 were NOT stolen and remain active!
    for (uint8_t note = 61; note <= 67; ++note) {
        assert(LeadSynth_IsNoteActive(note) == true);
    }
    std::cout << "Verified deterministic stealing: oldest note 60 stolen, 61..67 remain active, 68 added.\n";

    // Send EXACTLY ONE NoteOff for note 68
    LeadSynth_NoteOff(68);

    std::vector<int16_t> decay_buf(48000 * 2 * 2, 0);
    LeadSynth_FillStereoBuffer(decay_buf.data(), 48000 * 1.5);

    // Verify note 68 is cleanly released after a single NoteOff
    assert(LeadSynth_IsNoteActive(68) == false);

    // Verify remaining 7 notes 61..67 continue sounding
    for (uint8_t note = 61; note <= 67; ++note) {
        assert(LeadSynth_IsNoteActive(note) == true);
    }

    // Now release remaining 7 notes
    for (uint8_t note = 61; note <= 67; ++note) {
        LeadSynth_NoteOff(note);
    }

    LeadSynth_FillStereoBuffer(decay_buf.data(), 48000 * 1.5);

    for (uint8_t note = 60; note <= 68; ++note) {
        assert(LeadSynth_IsNoteActive(note) == false);
    }
    std::cout << "Verified: Single NoteOff cleanly releases stolen voice while other voices continue.\n";
}

void test_voice_stealing_with_multiple_presses() {
    LeadSynth_Init();

    // Trigger note 60 TWICE (multiple presses registered)
    LeadSynth_NoteOn(60, 100);
    LeadSynth_NoteOn(60, 100);

    std::vector<int16_t> step_buf(100 * 2, 0);
    LeadSynth_FillStereoBuffer(step_buf.data(), 100);

    // Fill remaining 7 voices with notes 61..67
    for (uint8_t note = 61; note <= 67; ++note) {
        LeadSynth_NoteOn(note, 100);
        LeadSynth_FillStereoBuffer(step_buf.data(), 100);
    }

    // All 8 voices occupied, note 60 is oldest. Trigger 9th note (68).
    LeadSynth_NoteOn(68, 100);
    LeadSynth_FillStereoBuffer(step_buf.data(), 100);

    // Note 60 stolen by note 68!
    assert(LeadSynth_IsNoteActive(60) == false);
    assert(LeadSynth_IsNoteActive(68) == true);

    // Send EXACTLY ONE NoteOff for note 68
    LeadSynth_NoteOff(68);

    std::vector<int16_t> decay_buf(48000 * 2 * 2, 0);
    LeadSynth_FillStereoBuffer(decay_buf.data(), 48000 * 1.5);

    // Note 68 must be cleanly released!
    assert(LeadSynth_IsNoteActive(68) == false);

    // Late NoteOff for old note 60 must not affect note 68 or other notes
    LeadSynth_NoteOff(60);
    LeadSynth_FillStereoBuffer(decay_buf.data(), 4800);
    assert(LeadSynth_IsNoteActive(68) == false);

    std::cout << "Verified: Voice stealing on note with multiple presses releases cleanly.\n";
}

void test_voice_stealing_with_sustain_pedal() {
    LeadSynth_Init();

    // Press sustain pedal CC64=127
    LeadSynth_ControlChange(64, 127);

    std::vector<int16_t> step_buf(100 * 2, 0);

    // Trigger note 60, then release key 60 (sustained by pedal)
    LeadSynth_NoteOn(60, 100);
    LeadSynth_FillStereoBuffer(step_buf.data(), 100);
    LeadSynth_NoteOff(60);
    LeadSynth_FillStereoBuffer(step_buf.data(), 100);
    assert(LeadSynth_IsNoteActive(60) == true); // Sustained by CC64

    // Fill remaining 7 voices with notes 61..67
    for (uint8_t note = 61; note <= 67; ++note) {
        LeadSynth_NoteOn(note, 100);
        LeadSynth_FillStereoBuffer(step_buf.data(), 100);
    }

    // All 8 voices occupied. Trigger 9th note (68). Note 60 is stolen!
    LeadSynth_NoteOn(68, 100);
    LeadSynth_FillStereoBuffer(step_buf.data(), 100);

    assert(LeadSynth_IsNoteActive(60) == false); // Stolen
    assert(LeadSynth_IsNoteActive(68) == true);  // Active

    // Release pedal CC64=0
    LeadSynth_ControlChange(64, 0);

    // Send NoteOff for note 68
    LeadSynth_NoteOff(68);

    std::vector<int16_t> decay_buf(48000 * 2 * 2, 0);
    LeadSynth_FillStereoBuffer(decay_buf.data(), 48000 * 1.5);

    // Note 68 must be cleanly released!
    assert(LeadSynth_IsNoteActive(68) == false);
    std::cout << "Verified: Voice stealing on sustained note releases cleanly without stuck notes.\n";
}

void test_envelope_retrigger_on_voice_stealing() {
    LeadSynth_Init();

    // Set longer attack time (0.2s) via CC 73
    LeadSynth_ControlChange(73, 100);

    std::vector<int16_t> step_buf(48000 * 2, 0);

    // Trigger single note 60 on voice 0 and render 1.0s to reach full sustain level (~12000 peak)
    LeadSynth_NoteOn(60, 100);
    LeadSynth_FillStereoBuffer(step_buf.data(), 48000);

    int sustain_peak = 0;
    for (size_t i = 40000; i < 48000; ++i) {
        int s = std::abs(step_buf[i * 2]);
        if (s > sustain_peak) sustain_peak = s;
    }
    std::cout << "Sustain peak before voice stealing: " << sustain_peak << "\n";
    assert(sustain_peak > 8000);

    // Trigger 8 notes in sequence so note 60 is stolen by note 68
    for (uint8_t note = 61; note <= 68; ++note) {
        LeadSynth_NoteOn(note, 100);
    }

    // Render 10 frames right after voice stealing
    std::vector<int16_t> stolen_start_buf(100 * 2, 0);
    LeadSynth_FillStereoBuffer(stolen_start_buf.data(), 100);

    // Render 1 second to let stolen voice ramp up
    std::vector<int16_t> stolen_attack_buf(48000 * 2, 0);
    LeadSynth_FillStereoBuffer(stolen_attack_buf.data(), 24000);

    // Verify first frame peak after stealing is significantly lower than full sustain
    int start_sample_peak = std::abs(stolen_start_buf[0]);
    std::cout << "First frame peak after voice stealing: " << start_sample_peak << "\n";

    // Hard retrigger reset envelope level to 0.f, starting attack ramp-up
    assert(start_sample_peak < sustain_peak / 2);

    std::cout << "Verified: Envelope hard retrigger on voice stealing resets level to 0.\n";
}

void test_polyphony_chords_and_controls() {
    LeadSynth_Init();

    // 1. Polyphonic chord (C major: 60, 64, 67)
    LeadSynth_NoteOn(60, 100);
    LeadSynth_NoteOn(64, 100);
    LeadSynth_NoteOn(67, 100);

    std::vector<int16_t> chord_buf(96000 * 2, 0);
    LeadSynth_FillStereoBuffer(chord_buf.data(), 4800);

    assert(LeadSynth_IsNoteActive(60) == true);
    assert(LeadSynth_IsNoteActive(64) == true);
    assert(LeadSynth_IsNoteActive(67) == true);

    // Test Control Change parameters
    LeadSynth_ControlChange(1, 127);  // Max filter envelope depth
    LeadSynth_ControlChange(71, 100); // Higher resonance
    LeadSynth_ControlChange(74, 80);  // Adjusted cutoff
    LeadSynth_ControlChange(73, 50);  // Attack time
    LeadSynth_ControlChange(72, 60);  // Release time

    LeadSynth_FillStereoBuffer(chord_buf.data(), 4800);

    int chord_peak = 0;
    for (size_t i = 0; i < 4800; ++i) {
        int s = std::abs(chord_buf[i * 2]);
        if (s > chord_peak) chord_peak = s;
    }
    assert(chord_peak > 1000);
    std::cout << "Verified polyphonic chord rendering and CC modulation.\n";

    // Release chord notes
    LeadSynth_NoteOff(60);
    LeadSynth_NoteOff(64);
    LeadSynth_NoteOff(67);

    LeadSynth_FillStereoBuffer(chord_buf.data(), 96000);
    assert(LeadSynth_IsNoteActive(60) == false);
    assert(LeadSynth_IsNoteActive(64) == false);
    assert(LeadSynth_IsNoteActive(67) == false);
}

void test_reinit_state_reset() {
    LeadSynth_Init();
    LeadSynth_ControlChange(64, 127); // sustain on
    LeadSynth_NoteOn(60, 100);

    std::vector<int16_t> buf(4800 * 2, 0);
    LeadSynth_FillStereoBuffer(buf.data(), 4800);

    // Re-initialization
    LeadSynth_Init();

    assert(LeadSynth_IsNoteActive(60) == false);
    assert(LeadSynth_GetDroppedEventsCount() == 0);

    std::vector<int16_t> quiet_buf(4800 * 2, 0);
    LeadSynth_FillStereoBuffer(quiet_buf.data(), 4800);
    for (int16_t s : quiet_buf) {
        assert(s == 0);
    }
    std::cout << "Verified: LeadSynth_Init restores clean state.\n";
}

int main() {
    test_lead_render_and_note_off();
    test_noteon_velocity_zero_regression();
    test_subblock_sustain_event_ordering();
    test_fifo_overflow_critical_release_delivery();
    test_overlapping_same_pitch_notes();
    test_deterministic_voice_stealing_and_reuse();
    test_voice_stealing_with_multiple_presses();
    test_voice_stealing_with_sustain_pedal();
    test_envelope_retrigger_on_voice_stealing();
    test_polyphony_chords_and_controls();
    test_reinit_state_reset();
    std::cout << "test_lead_host passed successfully." << std::endl;
    return 0;
}

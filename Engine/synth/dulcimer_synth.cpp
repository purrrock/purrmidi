#include "dulcimer_synth.h"
#include "synth_arena.h"
#include "synth_common.h"
#include "elements/dsp/resonator.h"
#include "elements/dsp/exciter.h"
#include "stmlib/utils/random.h"
#include "stmlib/dsp/dsp.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>

#define DULCIMER_SAMPLE_RATE     48000.0f
#define DULCIMER_VOICES          4
#define DULCIMER_MAX_MODES       24
#define DULCIMER_BLOCK_SIZE      16
#define EVENT_FIFO_SIZE          32
#define NOTE_ON_FIFO_LIMIT       28
#define DULCIMER_MASTER_GAIN     0.80f

enum DulcimerEventType : uint8_t {
    EVENT_NOTE_ON,
    EVENT_NOTE_OFF,
    EVENT_CONTROL_CHANGE
};

struct DulcimerEvent {
    DulcimerEventType type;
    uint8_t param1; // note number or CC control
    uint8_t param2; // velocity or CC value
    float freq;
    float vel;
};

struct DulcimerVoice {
    elements::Resonator resonator;
    elements::Exciter exciter;
    std::atomic<int32_t> voice_state{-1}; // -1 = inactive, 0..127 = active note number
    float freq{220.0f / DULCIMER_SAMPLE_RATE};
    float level_ema{0.0f};
    uint32_t trigger_age{0};
    uint8_t press_count{0};
    bool pending_trig{false};
    bool gate{false};
    bool sustain_held{false};
};

// Голоса лежат в общей арене памяти (synth_arena.h): одновременно звучит один движок. Указатель устанавливается
// в DulcimerSynth_Init(); если арену забрал другой движок, SynthArena_IsOwner(&voices) == false.
static DulcimerVoice* voices = nullptr;

static uint32_t global_trigger_counter = 0;

// User controllable parameters via MIDI CC
static std::atomic<float> user_geometry{0.35f};   // Inharmonicity / structure
static std::atomic<float> user_brightness{0.65f}; // Brightness
static std::atomic<float> user_damping{0.30f};    // Damping
static std::atomic<float> user_position{0.40f};   // Strike position
static std::atomic<float> user_timbre{0.70f};     // Exciter mallet timbre

// Очередь событий, учёт нажатий и протокол аварийного восстановления (общий компонент, см. synth_common.h)
static synth::NoteInput<DulcimerEvent, EVENT_FIFO_SIZE, NOTE_ON_FIFO_LIMIT> input;
static bool audio_sustain_pedal = false;

static inline float clamp_f(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static inline float mtof_norm(uint8_t note) {
    float f_hz = 440.0f * std::pow(2.0f, ((float)note - 69.0f) * (1.0f / 12.0f));
    return f_hz / DULCIMER_SAMPLE_RATE;
}

extern "C" {

void DulcimerSynth_Init(void) {
    // Голоса создаются в общей арене заново: данные прежнего владельца (другого движка) стираются.
    voices = SynthArena_NewArray<DulcimerVoice, DULCIMER_VOICES>(&voices);

    global_trigger_counter = 0;
    audio_sustain_pedal = false;

    for (int i = 0; i < DULCIMER_VOICES; i++) {
        voices[i].resonator.Init();
        voices[i].exciter.Init();

        voices[i].voice_state.store(-1, std::memory_order_relaxed);
        voices[i].freq         = 220.0f / DULCIMER_SAMPLE_RATE;
        voices[i].level_ema    = 0.0f;
        voices[i].trigger_age  = 0;
        voices[i].press_count  = 0;
        voices[i].pending_trig = false;
        voices[i].gate         = false;
        voices[i].sustain_held = false;

        voices[i].resonator.set_frequency(voices[i].freq);
        voices[i].resonator.set_geometry(0.35f);
        voices[i].resonator.set_brightness(0.65f);
        voices[i].resonator.set_damping(0.30f);
        voices[i].resonator.set_position(0.40f);
        voices[i].resonator.set_resolution(DULCIMER_MAX_MODES);

        voices[i].exciter.set_model(elements::EXCITER_MODEL_MALLET);
        voices[i].exciter.set_parameter(0.30f);
        voices[i].exciter.set_timbre(0.70f);
    }

    user_geometry.store(0.35f, std::memory_order_relaxed);
    user_brightness.store(0.65f, std::memory_order_relaxed);
    user_damping.store(0.30f, std::memory_order_relaxed);
    user_position.store(0.40f, std::memory_order_relaxed);
    user_timbre.store(0.70f, std::memory_order_relaxed);

    input.Reset();
}

void DulcimerSynth_NoteOn(uint8_t midi_note, uint8_t velocity) {
    if (velocity == 0) {
        DulcimerSynth_NoteOff(midi_note);
        return;
    }

    if (midi_note >= 128) return;

    float norm_freq = mtof_norm(midi_note);
    float norm_vel  = clamp_f((float)velocity * (1.0f / 127.0f), 0.0f, 1.0f);

    DulcimerEvent event;
    event.type   = EVENT_NOTE_ON;
    event.param1 = midi_note;
    event.param2 = velocity;
    event.freq   = norm_freq;
    event.vel    = norm_vel;

    input.NoteOn(midi_note, event);
}

void DulcimerSynth_NoteOff(uint8_t midi_note) {
    if (midi_note >= 128) return;

    DulcimerEvent event;
    event.type   = EVENT_NOTE_OFF;
    event.param1 = midi_note;
    event.param2 = 0;
    event.freq   = 0.0f;
    event.vel    = 0.0f;

    input.NoteOff(midi_note, event);
}

void DulcimerSynth_ControlChange(uint8_t cc_num, uint8_t value) {
    float norm = (float)value * (1.0f / 127.0f);

    switch (cc_num) {
        case 1:  // Modulation wheel / Timbre
        case 73: // Attack / Mallet hardness
            user_timbre.store(0.20f + norm * 0.79f, std::memory_order_relaxed);
            break;

        case 71: // Filter resonance / Structure / Geometry
            user_geometry.store(0.10f + norm * 0.80f, std::memory_order_relaxed);
            break;

        case 72: // Release time / Damping
            user_damping.store(0.05f + (1.0f - norm) * 0.70f, std::memory_order_relaxed);
            break;

        case 74: // Brightness
            user_brightness.store(0.10f + norm * 0.88f, std::memory_order_relaxed);
            break;

        case 70:
        case 75: // Strike position
            user_position.store(0.05f + norm * 0.90f, std::memory_order_relaxed);
            break;

        case 64: { // Sustain pedal
            DulcimerEvent event;
            event.type   = EVENT_CONTROL_CHANGE;
            event.param1 = cc_num;
            event.param2 = value;
            event.freq   = 0.0f;
            event.vel    = 0.0f;
            input.Critical(event);
            break;
        }

        default:
            break;
    }
}

void DulcimerSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_samples) {
    if (!SynthArena_IsOwner(&voices)) {     // арену забрал другой движок или Init() ещё не вызывался
        std::memset(buffer, 0, (size_t)num_samples * 2U * sizeof(int16_t));
        return;
    }

    // 1. Recovery state machine checks (см. synth_common.h, NoteInput::ServiceRecovery)
    input.ServiceRecovery([]() {
        audio_sustain_pedal = false;

        for (int i = 0; i < DULCIMER_VOICES; i++) {
            voices[i].press_count  = 0;
            voices[i].sustain_held = false;
            voices[i].gate         = false;
            voices[i].voice_state.store(-1, std::memory_order_relaxed);
        }
    });

    // 2. Process pending events in FIFO
    DulcimerEvent event;
    while (input.Pop(event)) {
        if (event.type == EVENT_NOTE_ON) {
            int target_voice = -1;

            // Search for voice already playing this note
            for (int i = 0; i < DULCIMER_VOICES; i++) {
                if (voices[i].voice_state.load(std::memory_order_relaxed) == (int32_t)event.param1) {
                    target_voice = i;
                    break;
                }
            }

            // Search for free voice
            if (target_voice == -1) {
                for (int i = 0; i < DULCIMER_VOICES; i++) {
                    if (voices[i].voice_state.load(std::memory_order_relaxed) == -1) {
                        target_voice = i;
                        break;
                    }
                }
            }

            // Steal oldest voice if all voices busy
            if (target_voice == -1) {
                uint32_t max_age_diff = 0;
                target_voice = 0;
                for (int i = 0; i < DULCIMER_VOICES; i++) {
                    uint32_t age_diff = global_trigger_counter - voices[i].trigger_age;
                    if (age_diff >= max_age_diff) {
                        max_age_diff = age_diff;
                        target_voice = i;
                    }
                }

                // Cleanly release stolen voice's previous note press count if stolen
                int32_t stolen_note = voices[target_voice].voice_state.load(std::memory_order_relaxed);
                if (stolen_note >= 0 && stolen_note < 128) {
                    input.ReleasePress((uint8_t)stolen_note);
                }
            }

            DulcimerVoice& v = voices[target_voice];
            v.freq         = event.freq;
            v.level_ema    = 1.0f;
            v.trigger_age  = ++global_trigger_counter;
            v.pending_trig = true;
            v.gate         = true;
            v.sustain_held = false;

            // If reusing same voice or stolen voice, reassign state cleanly
            int32_t cur_state = v.voice_state.load(std::memory_order_relaxed);
            if (cur_state == (int32_t)event.param1) {
                if (v.press_count < 255) v.press_count++;
            } else {
                v.press_count = 1;
                v.voice_state.store((int32_t)event.param1, std::memory_order_release);
            }

            v.resonator.set_frequency(event.freq);
            v.exciter.set_model(elements::EXCITER_MODEL_MALLET);
            v.exciter.set_parameter(0.10f + 0.80f * event.vel);
            v.exciter.set_timbre(user_timbre.load(std::memory_order_relaxed));

        } else if (event.type == EVENT_NOTE_OFF) {
            uint8_t note = event.param1;
            for (int i = 0; i < DULCIMER_VOICES; i++) {
                if (voices[i].voice_state.load(std::memory_order_relaxed) == (int32_t)note && voices[i].press_count > 0) {
                    voices[i].press_count--;
                    if (voices[i].press_count == 0) {
                        if (audio_sustain_pedal) {
                            voices[i].sustain_held = true;
                        } else {
                            voices[i].sustain_held = false;
                            voices[i].gate = false;
                        }
                    }
                }
            }
        } else if (event.type == EVENT_CONTROL_CHANGE) {
            if (event.param1 == 64) {
                bool pedal_on = (event.param2 >= 64);
                audio_sustain_pedal = pedal_on;
                if (!pedal_on) {
                    for (int i = 0; i < DULCIMER_VOICES; i++) {
                        voices[i].sustain_held = false;
                        if (voices[i].press_count == 0) {
                            voices[i].gate = false;
                        }
                    }
                }
            }
        }
    }

    // 3. Render audio block in sub-blocks of DULCIMER_BLOCK_SIZE
    uint32_t samples_remaining = num_samples;
    int16_t *out_ptr = buffer;

    float cur_geom   = user_geometry.load(std::memory_order_relaxed);
    float cur_bright = user_brightness.load(std::memory_order_relaxed);
    float cur_damp   = user_damping.load(std::memory_order_relaxed);
    float cur_pos    = user_position.load(std::memory_order_relaxed);
    float cur_timb   = user_timbre.load(std::memory_order_relaxed);

    while (samples_remaining > 0) {
        uint32_t chunk = std::min((uint32_t)DULCIMER_BLOCK_SIZE, samples_remaining);

        float mix_l[DULCIMER_BLOCK_SIZE] = {0.0f};
        float mix_r[DULCIMER_BLOCK_SIZE] = {0.0f};
        uint32_t active_count = 0;

        for (int i = 0; i < DULCIMER_VOICES; i++) {
            DulcimerVoice& v = voices[i];
            if (v.voice_state.load(std::memory_order_relaxed) < 0) {
                continue;
            }

            v.resonator.set_geometry(cur_geom);
            v.resonator.set_brightness(cur_bright);
            v.resonator.set_damping(cur_damp);
            v.resonator.set_position(cur_pos);
            v.exciter.set_timbre(cur_timb);

            uint8_t flags = 0;
            if (v.pending_trig) {
                flags |= elements::EXCITER_FLAG_RISING_EDGE;
                v.pending_trig = false; // Issue RISING_EDGE strictly on the first sub-block of attack
            }
            if (v.gate || v.sustain_held) {
                flags |= elements::EXCITER_FLAG_GATE;
            } else {
                flags |= elements::EXCITER_FLAG_FALLING_EDGE;
            }

            float exciter_out[DULCIMER_BLOCK_SIZE];
            float center_out[DULCIMER_BLOCK_SIZE];
            float side_out[DULCIMER_BLOCK_SIZE];
            float bow_strength[DULCIMER_BLOCK_SIZE] = {0.0f};

            v.exciter.Process(flags, exciter_out, chunk);
            v.resonator.Process(bow_strength, exciter_out, center_out, side_out, chunk);

            float block_energy = 0.0f;
            for (uint32_t k = 0; k < chunk; k++) {
                float l = center_out[k] + side_out[k];
                float r = center_out[k] - side_out[k];
                mix_l[k] += l;
                mix_r[k] += r;

                float sample_mag = std::abs(l) + std::abs(r);
                if (sample_mag > block_energy) {
                    block_energy = sample_mag;
                }
            }

            v.level_ema = 0.95f * v.level_ema + 0.05f * block_energy;

            if (v.press_count == 0 && !v.sustain_held && v.level_ema < 0.0001f) {
                v.voice_state.store(-1, std::memory_order_release);
                v.press_count  = 0;
                v.sustain_held = false;
                v.gate         = false;
            } else {
                active_count++;
            }
        }

        // Dynamic gain scaling to prevent clipping on polyphonic chords
        float gain = DULCIMER_MASTER_GAIN;
        if (active_count > 1) {
            gain /= std::sqrt((float)active_count);
        }

        for (uint32_t k = 0; k < chunk; k++) {
            float sample_l = mix_l[k] * gain * 32767.0f;
            float sample_r = mix_r[k] * gain * 32767.0f;

            sample_l = clamp_f(sample_l, -32768.0f, 32767.0f);
            sample_r = clamp_f(sample_r, -32768.0f, 32767.0f);

            *out_ptr++ = (int16_t)sample_l;
            *out_ptr++ = (int16_t)sample_r;
        }

        samples_remaining -= chunk;
    }
}

} // extern "C"

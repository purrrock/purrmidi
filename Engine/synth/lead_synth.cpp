#include "lead_synth.h"
#include "daisysp.h"
#include "Synthesis/oscillator.h"
#include "Filters/svf.h"
#include "Control/adsr.h"
#include "Utility/dsp.h"

#include <atomic>
#include <cmath>
#include <cstring>

using namespace daisysp;

#define LEAD_SAMPLE_RATE       48000.0f
#define LEAD_MASTER_GAIN       0.50f   // Запас по динамическому диапазону для 8 голосов
#define EVENT_FIFO_SIZE        32      // Буфер lock-free SPSC для событий
#define NOTE_ON_FIFO_LIMIT     28      // Резерв слотов для отпусканий нот и CC
#define LEAD_OUTPUT_SCALE      (LEAD_MASTER_GAIN * 32767.0f)

enum LeadEventType : uint8_t {
    EVENT_NOTE_ON,
    EVENT_NOTE_OFF,
    EVENT_CONTROL_CHANGE
};

struct LeadEvent {
    LeadEventType type;
    uint8_t param1; // note or CC control
    uint8_t param2; // velocity or CC value
    float freq;
    float velocity_gain;
};

struct LeadVoice {
    Oscillator osc;
    Svf filter;
    Adsr amp_env;
    Adsr filter_env;

    std::atomic<int32_t> voice_state{-1}; // -1 = свободен, 0..127 = номер ноты
    uint32_t trigger_age{0};
    uint8_t press_count{0};
    float velocity_gain{1.0f};
    bool gate{false};
    bool sustain_held{false};
};

// Буфер голосов размещается в секции .dma_buffer (AXI SRAM) для экономии DTCMRAM на STM32H7
#if defined(__GNUC__) && !defined(__EMSCRIPTEN__) && defined(__arm__)
static LeadVoice voices[LEAD_VOICES] __attribute__((section(".dma_buffer"), aligned(32)));
#else
static LeadVoice voices[LEAD_VOICES];
#endif

static uint32_t global_trigger_counter = 0;

// Параметры субтрактивного синтезатора (управляются через MIDI CC)
static std::atomic<float> filter_cutoff_base{3000.0f}; // Базовая частота среза LPF (Гц)
static std::atomic<float> filter_res{0.35f};           // Резонанс LPF (0..1)
static std::atomic<float> filter_env_amount{4000.0f};  // Глубина модуляции среза огибающей (Гц)

// Параметры огибающей
static std::atomic<float> env_attack_sec{0.005f};
static std::atomic<float> env_decay_sec{0.30f};
static std::atomic<float> env_sustain_lvl{0.60f};
static std::atomic<float> env_release_sec{0.25f};

static LeadEvent event_fifo[EVENT_FIFO_SIZE];
static std::atomic<uint32_t> fifo_head{0};
static std::atomic<uint32_t> fifo_tail{0};

static std::atomic<uint8_t> global_note_press_count[128];
static bool audio_sustain_pedal = false;

// Протокол восстановления при переполнении FIFO
enum RecoveryState : uint32_t {
    RECOVERY_NORMAL = 0,
    RECOVERY_REQUESTED = 1,
    RECOVERY_IN_PROGRESS = 2
};

static std::atomic<uint32_t> recovery_state{RECOVERY_NORMAL};
static std::atomic<uint32_t> active_producer_ops{0};

static inline float clamp_f(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static bool event_fifo_push_note_on(const LeadEvent& event) {
    uint32_t head = fifo_head.load(std::memory_order_relaxed);
    uint32_t tail = fifo_tail.load(std::memory_order_acquire);

    if (head - tail >= NOTE_ON_FIFO_LIMIT) {
        return false;
    }

    event_fifo[head & (EVENT_FIFO_SIZE - 1)] = event;
    fifo_head.store(head + 1, std::memory_order_release);
    return true;
}

static bool event_fifo_push_critical(const LeadEvent& event) {
    uint32_t head = fifo_head.load(std::memory_order_relaxed);
    uint32_t tail = fifo_tail.load(std::memory_order_acquire);

    if (head - tail >= EVENT_FIFO_SIZE) {
        return false;
    }

    event_fifo[head & (EVENT_FIFO_SIZE - 1)] = event;
    fifo_head.store(head + 1, std::memory_order_release);
    return true;
}

static bool event_fifo_pop(LeadEvent& event) {
    uint32_t tail = fifo_tail.load(std::memory_order_relaxed);
    uint32_t head = fifo_head.load(std::memory_order_acquire);

    if (head == tail) {
        return false;
    }

    event = event_fifo[tail & (EVENT_FIFO_SIZE - 1)];
    fifo_tail.store(tail + 1, std::memory_order_release);
    return true;
}

void LeadSynth_Init(void) {
    global_trigger_counter = 0;
    audio_sustain_pedal = false;

    filter_cutoff_base.store(3000.0f, std::memory_order_relaxed);
    filter_res.store(0.35f, std::memory_order_relaxed);
    filter_env_amount.store(4000.0f, std::memory_order_relaxed);

    env_attack_sec.store(0.005f, std::memory_order_relaxed);
    env_decay_sec.store(0.30f, std::memory_order_relaxed);
    env_sustain_lvl.store(0.60f, std::memory_order_relaxed);
    env_release_sec.store(0.25f, std::memory_order_relaxed);

    for (int i = 0; i < LEAD_VOICES; i++) {
        LeadVoice& v = voices[i];
        v.osc.Init(LEAD_SAMPLE_RATE);
        v.osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SAW);
        v.osc.SetAmp(1.0f);

        v.filter.Init(LEAD_SAMPLE_RATE);
        v.filter.SetDrive(0.1f);
        v.filter.SetRes(0.35f);

        v.amp_env.Init(LEAD_SAMPLE_RATE);
        v.amp_env.SetAttackTime(0.005f);
        v.amp_env.SetDecayTime(0.30f);
        v.amp_env.SetSustainLevel(0.60f);
        v.amp_env.SetReleaseTime(0.25f);

        v.filter_env.Init(LEAD_SAMPLE_RATE);
        v.filter_env.SetAttackTime(0.005f);
        v.filter_env.SetDecayTime(0.35f);
        v.filter_env.SetSustainLevel(0.40f);
        v.filter_env.SetReleaseTime(0.25f);

        v.voice_state.store(-1, std::memory_order_relaxed);
        v.trigger_age = 0;
        v.press_count = 0;
        v.velocity_gain = 1.0f;
        v.gate = false;
        v.sustain_held = false;
    }

    for (int i = 0; i < 128; i++) {
        global_note_press_count[i].store(0, std::memory_order_relaxed);
    }

    active_producer_ops.store(0, std::memory_order_relaxed);
    recovery_state.store(RECOVERY_NORMAL, std::memory_order_relaxed);
    fifo_head.store(0, std::memory_order_relaxed);
    fifo_tail.store(0, std::memory_order_relaxed);
}

void LeadSynth_NoteOn(uint8_t midi_note, uint8_t velocity) {
    if (velocity == 0) {
        LeadSynth_NoteOff(midi_note);
        return;
    }
    if (midi_note >= 128) return;

    active_producer_ops.fetch_add(1, std::memory_order_seq_cst);
    if (recovery_state.load(std::memory_order_seq_cst) != RECOVERY_NORMAL) {
        active_producer_ops.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }

    uint8_t count = global_note_press_count[midi_note].load(std::memory_order_relaxed);
    while (count < 255) {
        if (global_note_press_count[midi_note].compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
            break;
        }
    }

    float freq = mtof((float)midi_note);
    freq = clamp_f(freq, 20.0f, 18000.0f);

    float norm_vel = (float)velocity * (1.0f / 127.0f);
    float vel_gain = 0.20f + 0.80f * norm_vel;

    LeadEvent event;
    event.type = EVENT_NOTE_ON;
    event.param1 = midi_note;
    event.param2 = velocity;
    event.freq = freq;
    event.velocity_gain = vel_gain;

    if (!event_fifo_push_note_on(event)) {
        uint8_t cur_count = global_note_press_count[midi_note].load(std::memory_order_relaxed);
        while (cur_count > 0) {
            if (global_note_press_count[midi_note].compare_exchange_weak(cur_count, cur_count - 1, std::memory_order_relaxed)) {
                break;
            }
        }
    }

    active_producer_ops.fetch_sub(1, std::memory_order_seq_cst);
}

void LeadSynth_NoteOff(uint8_t midi_note) {
    if (midi_note >= 128) return;

    active_producer_ops.fetch_add(1, std::memory_order_seq_cst);
    if (recovery_state.load(std::memory_order_seq_cst) != RECOVERY_NORMAL) {
        active_producer_ops.fetch_sub(1, std::memory_order_seq_cst);
        return;
    }

    uint8_t count = global_note_press_count[midi_note].load(std::memory_order_relaxed);
    while (count > 0) {
        if (global_note_press_count[midi_note].compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) {
            break;
        }
    }

    LeadEvent event;
    event.type = EVENT_NOTE_OFF;
    event.param1 = midi_note;
    event.param2 = 0;

    if (!event_fifo_push_critical(event)) {
        recovery_state.store(RECOVERY_REQUESTED, std::memory_order_seq_cst);
    }

    active_producer_ops.fetch_sub(1, std::memory_order_seq_cst);
}

bool LeadSynth_IsNoteActive(uint8_t midi_note) {
    if (midi_note >= 128) return false;
    for (int i = 0; i < LEAD_VOICES; i++) {
        if (voices[i].voice_state.load(std::memory_order_acquire) == (int32_t)midi_note) {
            return true;
        }
    }
    return false;
}

void LeadSynth_ControlChange(uint8_t control, uint8_t value) {
    float norm = (float)value * (1.0f / 127.0f);

    switch (control) {
        case 1:  // Mod wheel -> увеличивает глубину и срез фильтра
            filter_cutoff_base.store(500.0f + norm * 8500.0f, std::memory_order_relaxed);
            break;

        case 71: // Cutoff Resonance
            filter_res.store(0.05f + norm * 0.90f, std::memory_order_relaxed);
            break;

        case 74: // Filter Cutoff Frequency
            filter_cutoff_base.store(200.0f + norm * 12000.0f, std::memory_order_relaxed);
            break;

        case 72: // Release Time
            env_release_sec.store(0.02f + norm * 2.0f, std::memory_order_relaxed);
            break;

        case 73: // Attack Time
            env_attack_sec.store(0.002f + norm * 1.5f, std::memory_order_relaxed);
            break;

        case 64: { // Sustain Pedal
            active_producer_ops.fetch_add(1, std::memory_order_seq_cst);
            if (recovery_state.load(std::memory_order_seq_cst) != RECOVERY_NORMAL) {
                active_producer_ops.fetch_sub(1, std::memory_order_seq_cst);
                break;
            }

            LeadEvent event;
            event.type = EVENT_CONTROL_CHANGE;
            event.param1 = control;
            event.param2 = value;

            if (!event_fifo_push_critical(event)) {
                recovery_state.store(RECOVERY_REQUESTED, std::memory_order_seq_cst);
            }

            active_producer_ops.fetch_sub(1, std::memory_order_seq_cst);
            break;
        }

        default:
            break;
    }
}

static int16_t LeadSynth_NextSample(void) {
    uint32_t rec_st = recovery_state.load(std::memory_order_seq_cst);
    if (rec_st != RECOVERY_NORMAL) {
        recovery_state.store(RECOVERY_IN_PROGRESS, std::memory_order_seq_cst);

        if (active_producer_ops.load(std::memory_order_seq_cst) == 0) {
            audio_sustain_pedal = false;

            for (int i = 0; i < LEAD_VOICES; i++) {
                voices[i].press_count = 0;
                voices[i].sustain_held = false;
                voices[i].gate = false;
                voices[i].voice_state.store(-1, std::memory_order_release);
            }

            for (int i = 0; i < 128; i++) {
                global_note_press_count[i].store(0, std::memory_order_relaxed);
            }

            uint32_t current_head = fifo_head.load(std::memory_order_relaxed);
            fifo_tail.store(current_head, std::memory_order_release);

            recovery_state.store(RECOVERY_NORMAL, std::memory_order_seq_cst);
        }
    }

    LeadEvent event;
    while (event_fifo_pop(event)) {
        if (event.type == EVENT_NOTE_ON) {
            int target_voice = -1;

            // 1. Голос с такой же нотой
            for (int i = 0; i < LEAD_VOICES; i++) {
                if (voices[i].voice_state.load(std::memory_order_relaxed) == (int32_t)event.param1) {
                    target_voice = i;
                    break;
                }
            }

            // 2. Свободный голос
            if (target_voice == -1) {
                for (int i = 0; i < LEAD_VOICES; i++) {
                    if (voices[i].voice_state.load(std::memory_order_relaxed) == -1) {
                        target_voice = i;
                        break;
                    }
                }
            }

            // 3. Вытеснение наиболе старого голоса
            if (target_voice == -1) {
                uint32_t max_age_diff = 0;
                target_voice = 0;
                for (int i = 0; i < LEAD_VOICES; i++) {
                    uint32_t age_diff = global_trigger_counter - voices[i].trigger_age;
                    if (age_diff >= max_age_diff) {
                        max_age_diff = age_diff;
                        target_voice = i;
                    }
                }
            }

            LeadVoice& v = voices[target_voice];
            v.osc.SetFreq(event.freq);
            v.velocity_gain = event.velocity_gain;
            v.trigger_age = ++global_trigger_counter;
            if (v.press_count < 255) v.press_count++;
            v.sustain_held = false;
            v.gate = true;

            float att = env_attack_sec.load(std::memory_order_relaxed);
            float dec = env_decay_sec.load(std::memory_order_relaxed);
            float sus = env_sustain_lvl.load(std::memory_order_relaxed);
            float rel = env_release_sec.load(std::memory_order_relaxed);

            v.amp_env.SetAttackTime(att);
            v.amp_env.SetDecayTime(dec);
            v.amp_env.SetSustainLevel(sus);
            v.amp_env.SetReleaseTime(rel);
            v.amp_env.Retrigger(false);

            v.filter_env.SetAttackTime(att);
            v.filter_env.SetDecayTime(dec + 0.05f);
            v.filter_env.SetSustainLevel(sus * 0.7f);
            v.filter_env.SetReleaseTime(rel);
            v.filter_env.Retrigger(false);

            v.voice_state.store((int32_t)event.param1, std::memory_order_release);

        } else if (event.type == EVENT_NOTE_OFF) {
            uint8_t note = event.param1;
            for (int i = 0; i < LEAD_VOICES; i++) {
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
                    for (int i = 0; i < LEAD_VOICES; i++) {
                        voices[i].sustain_held = false;
                        if (voices[i].press_count == 0) {
                            voices[i].gate = false;
                        }
                    }
                }
            }
        }
    }

    float mix_sum_f = 0.0f;
    uint32_t active_voices_count = 0;

    float base_cutoff = filter_cutoff_base.load(std::memory_order_relaxed);
    float res_val     = filter_res.load(std::memory_order_relaxed);
    float env_amt     = filter_env_amount.load(std::memory_order_relaxed);

    for (int i = 0; i < LEAD_VOICES; i++) {
        LeadVoice& v = voices[i];

        if (v.voice_state.load(std::memory_order_relaxed) >= 0) {
            bool gate = v.gate || v.sustain_held;
            float amp_val = v.amp_env.Process(gate);
            float f_env_val = v.filter_env.Process(gate);

            if (!v.amp_env.IsRunning()) {
                v.voice_state.store(-1, std::memory_order_release);
                v.press_count = 0;
                v.sustain_held = false;
                v.gate = false;
                continue;
            }

            float raw_osc = v.osc.Process();

            // Динамическая модуляция частоты среза LPF огибающей
            float cutoff = base_cutoff + f_env_val * env_amt;
            cutoff = clamp_f(cutoff, 30.0f, 15000.0f);

            v.filter.SetRes(res_val);
            v.filter.SetFreq(cutoff);
            v.filter.Process(raw_osc);

            float filtered_sample = v.filter.Low();
            float sample_f = filtered_sample * amp_val * v.velocity_gain;

            active_voices_count++;
            mix_sum_f += sample_f;
        }
    }

    float norm_gain = 1.0f;
    if (active_voices_count > 1) {
        norm_gain = 1.0f / (1.0f + 0.35f * (float)(active_voices_count - 1));
    }

    float output_sample_f = mix_sum_f * norm_gain * LEAD_OUTPUT_SCALE;

    if (output_sample_f > 32767.0f) {
        output_sample_f = 32767.0f;
    } else if (output_sample_f < -32768.0f) {
        output_sample_f = -32768.0f;
    }

    return (int16_t)output_sample_f;
}

void LeadSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames) {
    for (uint32_t i = 0; i < num_frames; i++) {
        int16_t sample = LeadSynth_NextSample();
        buffer[i * 2]     = sample; // L
        buffer[i * 2 + 1] = sample; // R
    }
}

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

// Параметры конфигурации аудио-тракта и FIFO
#define LEAD_SAMPLE_RATE          48000.0f
#define LEAD_MASTER_GAIN          0.50f   // Запас по динамическому диапазону для 8 голосов
#define EVENT_FIFO_SIZE           32      // Емкость кольцевого FIFO буфера
#define NOTE_ON_FIFO_LIMIT        28      // Порог резервирования слотов для Note Off и CC64
#define LEAD_OUTPUT_SCALE         (LEAD_MASTER_GAIN * 32767.0f)

// Диапазоны частот и генерации
#define LEAD_MIN_FREQ             20.0f
#define LEAD_MAX_FREQ             18000.0f

// По умолчанию и ограничения параметров синтеза (настраиваются через CC)
#define LEAD_DEFAULT_CUTOFF       3000.0f
#define LEAD_MIN_CUTOFF           30.0f
#define LEAD_MAX_CUTOFF           12000.0f

#define LEAD_DEFAULT_RES          0.35f
#define LEAD_MIN_RES              0.05f
#define LEAD_MAX_RES              0.95f

#define LEAD_DEFAULT_ENV_AMT      4000.0f
#define LEAD_MAX_ENV_AMT          10000.0f

#define LEAD_DEFAULT_ATTACK       0.005f
#define LEAD_MIN_ATTACK           0.002f
#define LEAD_MAX_ATTACK           1.500f

#define LEAD_DEFAULT_DECAY        0.300f
#define LEAD_DEFAULT_SUSTAIN      0.600f

#define LEAD_DEFAULT_RELEASE      0.250f
#define LEAD_MIN_RELEASE          0.020f
#define LEAD_MAX_RELEASE          2.000f

#define LEAD_VELOCITY_MIN_GAIN    0.200f
#define LEAD_VELOCITY_SCALE       0.800f

#define LEAD_FILTER_DECAY_OFFSET  0.050f
#define LEAD_FILTER_SUSTAIN_SCALE 0.700f

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
static std::atomic<float> filter_cutoff_base{LEAD_DEFAULT_CUTOFF};
static std::atomic<float> filter_res{LEAD_DEFAULT_RES};
static std::atomic<float> filter_env_amount{LEAD_DEFAULT_ENV_AMT};

// Параметры огибающей
static std::atomic<float> env_attack_sec{LEAD_DEFAULT_ATTACK};
static std::atomic<float> env_decay_sec{LEAD_DEFAULT_DECAY};
static std::atomic<float> env_sustain_lvl{LEAD_DEFAULT_SUSTAIN};
static std::atomic<float> env_release_sec{LEAD_DEFAULT_RELEASE};

static LeadEvent event_fifo[EVENT_FIFO_SIZE];
static std::atomic<uint32_t> fifo_head{0};
static std::atomic<uint32_t> fifo_tail{0};
static std::atomic<uint32_t> dropped_note_on_count{0};

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
        dropped_note_on_count.fetch_add(1, std::memory_order_relaxed);
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

    filter_cutoff_base.store(LEAD_DEFAULT_CUTOFF, std::memory_order_relaxed);
    filter_res.store(LEAD_DEFAULT_RES, std::memory_order_relaxed);
    filter_env_amount.store(LEAD_DEFAULT_ENV_AMT, std::memory_order_relaxed);

    env_attack_sec.store(LEAD_DEFAULT_ATTACK, std::memory_order_relaxed);
    env_decay_sec.store(LEAD_DEFAULT_DECAY, std::memory_order_relaxed);
    env_sustain_lvl.store(LEAD_DEFAULT_SUSTAIN, std::memory_order_relaxed);
    env_release_sec.store(LEAD_DEFAULT_RELEASE, std::memory_order_relaxed);

    for (int i = 0; i < LEAD_VOICES; i++) {
        LeadVoice& v = voices[i];
        v.osc.Init(LEAD_SAMPLE_RATE);
        v.osc.SetWaveform(Oscillator::WAVE_POLYBLEP_SAW);
        v.osc.SetAmp(1.0f);

        v.filter.Init(LEAD_SAMPLE_RATE);
        v.filter.SetDrive(0.1f);
        v.filter.SetRes(LEAD_DEFAULT_RES);

        v.amp_env.Init(LEAD_SAMPLE_RATE);
        v.amp_env.SetAttackTime(LEAD_DEFAULT_ATTACK);
        v.amp_env.SetDecayTime(LEAD_DEFAULT_DECAY);
        v.amp_env.SetSustainLevel(LEAD_DEFAULT_SUSTAIN);
        v.amp_env.SetReleaseTime(LEAD_DEFAULT_RELEASE);

        v.filter_env.Init(LEAD_SAMPLE_RATE);
        v.filter_env.SetAttackTime(LEAD_DEFAULT_ATTACK);
        v.filter_env.SetDecayTime(LEAD_DEFAULT_DECAY + LEAD_FILTER_DECAY_OFFSET);
        v.filter_env.SetSustainLevel(LEAD_DEFAULT_SUSTAIN * LEAD_FILTER_SUSTAIN_SCALE);
        v.filter_env.SetReleaseTime(LEAD_DEFAULT_RELEASE);

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
    dropped_note_on_count.store(0, std::memory_order_relaxed);
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
        dropped_note_on_count.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint8_t count = global_note_press_count[midi_note].load(std::memory_order_relaxed);
    while (count < 255) {
        if (global_note_press_count[midi_note].compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
            break;
        }
    }

    float freq = mtof((float)midi_note);
    freq = clamp_f(freq, LEAD_MIN_FREQ, LEAD_MAX_FREQ);

    float norm_vel = (float)velocity * (1.0f / 127.0f);
    float vel_gain = LEAD_VELOCITY_MIN_GAIN + LEAD_VELOCITY_SCALE * norm_vel;

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

uint32_t LeadSynth_GetDroppedEventsCount(void) {
    return dropped_note_on_count.load(std::memory_order_relaxed);
}

void LeadSynth_ControlChange(uint8_t control, uint8_t value) {
    float norm = (float)value * (1.0f / 127.0f);

    switch (control) {
        case 1:  // CC 1: Modulation Wheel -> глубина огибающей фильтра (0..10000 Гц)
            filter_env_amount.store(norm * LEAD_MAX_ENV_AMT, std::memory_order_relaxed);
            break;

        case 71: // CC 71: Resonance (0.05..0.95)
            filter_res.store(LEAD_MIN_RES + norm * (LEAD_MAX_RES - LEAD_MIN_RES), std::memory_order_relaxed);
            break;

        case 74: // CC 74: Filter Cutoff Frequency (30..12000 Гц)
            filter_cutoff_base.store(LEAD_MIN_CUTOFF + norm * (LEAD_MAX_CUTOFF - LEAD_MIN_CUTOFF), std::memory_order_relaxed);
            break;

        case 72: // CC 72: Release Time (0.02..2.02 с)
            env_release_sec.store(LEAD_MIN_RELEASE + norm * LEAD_MAX_RELEASE, std::memory_order_relaxed);
            break;

        case 73: // CC 73: Attack Time (0.002..1.502 с)
            env_attack_sec.store(LEAD_MIN_ATTACK + norm * LEAD_MAX_ATTACK, std::memory_order_relaxed);
            break;

        case 64: { // CC 64: Sustain Pedal
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

            // 1. Поиск голоса, уже играющего данную ноту
            for (int i = 0; i < LEAD_VOICES; i++) {
                if (voices[i].voice_state.load(std::memory_order_relaxed) == (int32_t)event.param1) {
                    target_voice = i;
                    break;
                }
            }

            bool is_retrigger = (target_voice != -1);

            // 2. Поиск свободного голоса
            if (target_voice == -1) {
                for (int i = 0; i < LEAD_VOICES; i++) {
                    if (voices[i].voice_state.load(std::memory_order_relaxed) == -1) {
                        target_voice = i;
                        break;
                    }
                }
            }

            // 3. Детерминированное вытеснение самого старого активного голоса
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
            if (is_retrigger) {
                if (v.press_count < 255) v.press_count++;
            } else {
                v.press_count = 1;
            }
            v.sustain_held = false;
            v.gate = true;

            float att = env_attack_sec.load(std::memory_order_relaxed);
            float dec = env_decay_sec.load(std::memory_order_relaxed);
            float sus = env_sustain_lvl.load(std::memory_order_relaxed);
            float rel = env_release_sec.load(std::memory_order_relaxed);

            bool hard_retrigger = !is_retrigger; // Жесткий перезапуск огибающей при выборе/вытеснении нового голоса

            v.amp_env.SetAttackTime(att);
            v.amp_env.SetDecayTime(dec);
            v.amp_env.SetSustainLevel(sus);
            v.amp_env.SetReleaseTime(rel);
            v.amp_env.Retrigger(hard_retrigger);
            v.amp_env.Process(true);

            v.filter_env.SetAttackTime(att);
            v.filter_env.SetDecayTime(dec + LEAD_FILTER_DECAY_OFFSET);
            v.filter_env.SetSustainLevel(sus * LEAD_FILTER_SUSTAIN_SCALE);
            v.filter_env.SetReleaseTime(rel);
            v.filter_env.Retrigger(hard_retrigger);
            v.filter_env.Process(true);

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
            cutoff = clamp_f(cutoff, LEAD_MIN_CUTOFF, LEAD_MAX_CUTOFF);

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

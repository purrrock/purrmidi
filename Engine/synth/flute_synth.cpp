#include "flute_synth.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>

// Константы конфигурации синтезатора флейты
#define FLUTE_SAMPLE_RATE         48000.0f
#define FLUTE_MASTER_GAIN         0.70f   // Запас по динамическому диапазону
#define BORE_DELAY_SIZE           1024    // Емкость линии задержки ствола (степень двойки, мин. частота ~46.8 Гц)
#define JET_DELAY_SIZE            512     // Емкость линии задержки струи (степень двойки)
#define EVENT_FIFO_SIZE           32      // Емкость кольцевого FIFO буфера
#define NOTE_ON_FIFO_LIMIT        28      // Порог резервирования слотов для критических событий (NoteOff, CC64)

// Диапазоны частот (MIDI нота 31 = G1 46.875 Гц ... нота 108 = C8 4186 Гц)
#define FLUTE_MIN_FREQ            46.875f
#define FLUTE_MAX_FREQ            18000.0f

// Параметры дыхания и огибающей по умолчанию
#define FLUTE_DEFAULT_BREATH      0.80f
#define FLUTE_DEFAULT_TURBULENCE  0.08f
#define FLUTE_DEFAULT_VIBRATO_DEP 0.02f
#define FLUTE_DEFAULT_VIBRATO_FREQ 5.5f
#define FLUTE_DEFAULT_JET_RATIO   0.32f
#define FLUTE_DEFAULT_DAMPING     0.35f
#define FLUTE_DEFAULT_ATTACK      0.020f
#define FLUTE_DEFAULT_RELEASE     0.150f

// Ограничения soft limit
#define FLUTE_LIMIT_KNEE          0.80f
#define FLUTE_LIMIT_CEIL          0.98f

enum FluteEventType : uint8_t {
    EVENT_NOTE_ON,
    EVENT_NOTE_OFF,
    EVENT_CONTROL_CHANGE
};

struct FluteEvent {
    FluteEventType type;
    uint8_t param1; // note or CC control
    uint8_t param2; // velocity or CC value
    float freq;
    float velocity_gain;
};

// Быстрая циклическая линия задержки с линейной интерполяцией
template <size_t SIZE>
class DelayLine {
public:
    DelayLine() { Reset(); }

    void Reset() {
        std::memset(buffer_, 0, sizeof(buffer_));
        write_pos_ = 0;
    }

    inline void Write(float sample) {
        buffer_[write_pos_] = sample;
        write_pos_ = (write_pos_ + 1) & (SIZE - 1);
    }

    inline float Read(float delay_samples) const {
        if (delay_samples < 1.0f) delay_samples = 1.0f;
        const float max_delay = (float)(SIZE - 2);
        if (delay_samples > max_delay) delay_samples = max_delay;

        float read_pos = (float)write_pos_ - delay_samples;
        while (read_pos < 0.0f) {
            read_pos += (float)SIZE;
        }

        uint32_t i0 = (uint32_t)read_pos;
        float frac = read_pos - (float)i0;
        uint32_t idx0 = i0 & (SIZE - 1);
        uint32_t idx1 = (idx0 + 1) & (SIZE - 1);

        return buffer_[idx0] + frac * (buffer_[idx1] - buffer_[idx0]);
    }

private:
    float buffer_[SIZE];
    uint32_t write_pos_{0};
};

// Простой генератор псевдослучайных чисел Xorshift32
static inline float FastRandom(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return (float)state * (2.0f / 4294967295.0f) - 1.0f;
}

// Нелинейная функция возбудителя Jet Table: f(x) = x^3 - x (с ограничением [-1, 1])
static inline float JetTable(float x) {
    float y = x * x * x - x;
    if (y > 1.0f) return 1.0f;
    if (y < -1.0f) return -1.0f;
    return y;
}

struct FluteVoice {
    DelayLine<BORE_DELAY_SIZE> bore_delay;
    DelayLine<JET_DELAY_SIZE> jet_delay;

    std::atomic<int32_t> voice_state{-1}; // -1 = свободен, 0..127 = номер ноты
    uint32_t trigger_age{0};
    uint8_t press_count{0};
    uint32_t prng_state{0x12345678};

    float target_freq{440.0f};
    float bore_length{100.0f};
    float velocity_gain{1.0f};

    float breath_env{0.0f};
    float filter_state{0.0f};
    float vibrato_phase{0.0f};

    bool gate{false};
    bool sustain_held{false};
};

// Таблица частот MIDI-нот 0..127 (предрассчитана)
static float kMidiToFreq[128];
static bool kMidiToFreqInitialized = false;

static void InitMidiToFreqTable(void) {
    if (kMidiToFreqInitialized) return;
    for (int i = 0; i < 128; i++) {
        kMidiToFreq[i] = 440.0f * std::pow(2.0f, (float)(i - 69) / 12.0f);
    }
    kMidiToFreqInitialized = true;
}

// Размещение буферов голосов в секции .ram_d2 (D2 SRAM) на STM32H7 во избежание переполнения AXI SRAM
#if defined(__GNUC__) && !defined(__EMSCRIPTEN__) && defined(__arm__)
static FluteVoice voices[FLUTE_VOICES] __attribute__((section(".ram_d2"), aligned(32)));
#else
static FluteVoice voices[FLUTE_VOICES];
#endif

static uint32_t global_trigger_counter = 0;

// Параметры синтезатора флейты (атомарны для предотвращения data race между main и ISR)
static std::atomic<float> cc_vibrato_depth{FLUTE_DEFAULT_VIBRATO_DEP};
static std::atomic<float> cc_turbulence{FLUTE_DEFAULT_TURBULENCE};
static std::atomic<float> cc_jet_ratio{FLUTE_DEFAULT_JET_RATIO};
static std::atomic<float> cc_bore_damping{FLUTE_DEFAULT_DAMPING};
static std::atomic<float> cc_release_sec{FLUTE_DEFAULT_RELEASE};
static std::atomic<float> cc_attack_sec{FLUTE_DEFAULT_ATTACK};

static FluteEvent event_fifo[EVENT_FIFO_SIZE];
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

static inline float soft_limit(float x) {
    float a = std::abs(x);
    if (a <= FLUTE_LIMIT_KNEE) return x;
    float room = FLUTE_LIMIT_CEIL - FLUTE_LIMIT_KNEE;
    float y = FLUTE_LIMIT_KNEE + room * std::tanh((a - FLUTE_LIMIT_KNEE) / room);
    return (x < 0.0f) ? -y : y;
}

static bool event_fifo_push_note_on(const FluteEvent& event) {
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

static bool event_fifo_push_critical(const FluteEvent& event) {
    uint32_t head = fifo_head.load(std::memory_order_relaxed);
    uint32_t tail = fifo_tail.load(std::memory_order_acquire);

    if (head - tail >= EVENT_FIFO_SIZE) {
        return false;
    }

    event_fifo[head & (EVENT_FIFO_SIZE - 1)] = event;
    fifo_head.store(head + 1, std::memory_order_release);
    return true;
}

static bool event_fifo_pop(FluteEvent& event) {
    uint32_t tail = fifo_tail.load(std::memory_order_relaxed);
    uint32_t head = fifo_head.load(std::memory_order_acquire);

    if (head == tail) {
        return false;
    }

    event = event_fifo[tail & (EVENT_FIFO_SIZE - 1)];
    fifo_tail.store(tail + 1, std::memory_order_release);
    return true;
}

extern "C" {

void FluteSynth_Init(void) {
    InitMidiToFreqTable();

    global_trigger_counter = 0;
    audio_sustain_pedal = false;

    cc_vibrato_depth.store(FLUTE_DEFAULT_VIBRATO_DEP, std::memory_order_relaxed);
    cc_turbulence.store(FLUTE_DEFAULT_TURBULENCE, std::memory_order_relaxed);
    cc_jet_ratio.store(FLUTE_DEFAULT_JET_RATIO, std::memory_order_relaxed);
    cc_bore_damping.store(FLUTE_DEFAULT_DAMPING, std::memory_order_relaxed);
    cc_release_sec.store(FLUTE_DEFAULT_RELEASE, std::memory_order_relaxed);
    cc_attack_sec.store(FLUTE_DEFAULT_ATTACK, std::memory_order_relaxed);

    for (int i = 0; i < FLUTE_VOICES; i++) {
        FluteVoice& v = voices[i];
        v.bore_delay.Reset();
        v.jet_delay.Reset();

        v.voice_state.store(-1, std::memory_order_relaxed);
        v.trigger_age = 0;
        v.press_count = 0;
        v.prng_state = 0x12345678 + i * 0x1000;

        v.target_freq = 440.0f;
        v.bore_length = FLUTE_SAMPLE_RATE / 440.0f - 2.0f;
        v.velocity_gain = 1.0f;

        v.breath_env = 0.0f;
        v.filter_state = 0.0f;
        v.vibrato_phase = 0.0f;

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

void FluteSynth_NoteOn(uint8_t midi_note, uint8_t velocity) {
    if (velocity == 0) {
        FluteSynth_NoteOff(midi_note);
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

    InitMidiToFreqTable();
    float freq = kMidiToFreq[midi_note];
    freq = clamp_f(freq, FLUTE_MIN_FREQ, FLUTE_MAX_FREQ);

    float norm_vel = (float)velocity * (1.0f / 127.0f);
    float vel_gain = 0.20f + 0.80f * norm_vel;

    FluteEvent event;
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

void FluteSynth_NoteOff(uint8_t midi_note) {
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

    FluteEvent event;
    event.type = EVENT_NOTE_OFF;
    event.param1 = midi_note;
    event.param2 = 0;

    if (!event_fifo_push_critical(event)) {
        recovery_state.store(RECOVERY_REQUESTED, std::memory_order_seq_cst);
    }

    active_producer_ops.fetch_sub(1, std::memory_order_seq_cst);
}

bool FluteSynth_IsNoteActive(uint8_t midi_note) {
    if (midi_note >= 128) return false;
    for (int i = 0; i < FLUTE_VOICES; i++) {
        if (voices[i].voice_state.load(std::memory_order_acquire) == (int32_t)midi_note) {
            return true;
        }
    }
    return false;
}

uint32_t FluteSynth_GetDroppedEventsCount(void) {
    return dropped_note_on_count.load(std::memory_order_relaxed);
}

void FluteSynth_ControlChange(uint8_t control, uint8_t value) {
    float norm = (float)value * (1.0f / 127.0f);

    switch (control) {
        case 1:  // CC 1: Mod Wheel -> Глубина вибрато (0..0.08)
            cc_vibrato_depth.store(norm * 0.08f, std::memory_order_relaxed);
            break;

        case 2:  // CC 2: Breath Controller -> Турбулентность/Шум
        case 11: // CC 11: Expression
        case 73: // CC 73: Attack / Breath Intensity
            cc_turbulence.store(norm * 0.25f, std::memory_order_relaxed);
            break;

        case 71: // CC 71: Resonance / Damping (0.05..0.85)
            cc_bore_damping.store(0.05f + norm * 0.80f, std::memory_order_relaxed);
            break;

        case 72: { // CC 72: Release Time (0.02..1.50 с)
            float rel = 0.02f + norm * 1.48f;
            cc_release_sec.store(rel, std::memory_order_relaxed);
            break;
        }

        case 74: // CC 74: Brightness / Jet Ratio (0.15..0.50)
            cc_jet_ratio.store(0.15f + norm * 0.35f, std::memory_order_relaxed);
            break;

        case 64: { // CC 64: Sustain Pedal
            active_producer_ops.fetch_add(1, std::memory_order_seq_cst);
            if (recovery_state.load(std::memory_order_seq_cst) != RECOVERY_NORMAL) {
                active_producer_ops.fetch_sub(1, std::memory_order_seq_cst);
                break;
            }

            FluteEvent event;
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

static int16_t FluteSynth_NextSample(void) {
    uint32_t rec_st = recovery_state.load(std::memory_order_seq_cst);
    if (rec_st != RECOVERY_NORMAL) {
        recovery_state.store(RECOVERY_IN_PROGRESS, std::memory_order_seq_cst);

        if (active_producer_ops.load(std::memory_order_seq_cst) == 0) {
            audio_sustain_pedal = false;

            for (int i = 0; i < FLUTE_VOICES; i++) {
                voices[i].press_count = 0;
                voices[i].sustain_held = false;
                voices[i].gate = false;
                voices[i].breath_env = 0.0f;
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

    FluteEvent event;
    while (event_fifo_pop(event)) {
        if (event.type == EVENT_NOTE_ON) {
            int target_voice = -1;

            for (int i = 0; i < FLUTE_VOICES; i++) {
                if (voices[i].voice_state.load(std::memory_order_relaxed) == (int32_t)event.param1) {
                    target_voice = i;
                    break;
                }
            }

            bool is_retrigger = (target_voice != -1);

            if (target_voice == -1) {
                for (int i = 0; i < FLUTE_VOICES; i++) {
                    if (voices[i].voice_state.load(std::memory_order_relaxed) == -1) {
                        target_voice = i;
                        break;
                    }
                }
            }

            if (target_voice == -1) {
                uint32_t max_age_diff = 0;
                target_voice = 0;
                for (int i = 0; i < FLUTE_VOICES; i++) {
                    uint32_t age_diff = global_trigger_counter - voices[i].trigger_age;
                    if (age_diff >= max_age_diff) {
                        max_age_diff = age_diff;
                        target_voice = i;
                    }
                }
            }

            FluteVoice& v = voices[target_voice];
            v.target_freq = event.freq;
            v.bore_length = FLUTE_SAMPLE_RATE / event.freq - 2.0f;
            v.velocity_gain = event.velocity_gain;
            v.trigger_age = ++global_trigger_counter;

            if (is_retrigger) {
                if (v.press_count < 255) v.press_count++;
            } else {
                v.press_count = 1;
                v.bore_delay.Reset();
                v.jet_delay.Reset();
                v.filter_state = 0.0f;
            }

            v.sustain_held = false;
            v.gate = true;

            v.voice_state.store((int32_t)event.param1, std::memory_order_release);

        } else if (event.type == EVENT_NOTE_OFF) {
            uint8_t note = event.param1;
            for (int i = 0; i < FLUTE_VOICES; i++) {
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
                    for (int i = 0; i < FLUTE_VOICES; i++) {
                        voices[i].sustain_held = false;
                        if (voices[i].press_count == 0) {
                            voices[i].gate = false;
                        }
                    }
                }
            }
        }
    }

    float mix_sum = 0.0f;
    uint32_t active_voices_count = 0;

    const float att_sec = cc_attack_sec.load(std::memory_order_relaxed);
    const float rel_sec = cc_release_sec.load(std::memory_order_relaxed);
    const float attack_rate = 1.0f / (att_sec * FLUTE_SAMPLE_RATE);
    const float release_rate = 1.0f / (rel_sec * FLUTE_SAMPLE_RATE);

    const float vib_depth = cc_vibrato_depth.load(std::memory_order_relaxed);
    const float turb_gain = cc_turbulence.load(std::memory_order_relaxed);
    const float jet_ratio = cc_jet_ratio.load(std::memory_order_relaxed);
    const float damping = cc_bore_damping.load(std::memory_order_relaxed);
    const float vib_step = (2.0f * 3.14159265f * FLUTE_DEFAULT_VIBRATO_FREQ) / FLUTE_SAMPLE_RATE;

    for (int i = 0; i < FLUTE_VOICES; i++) {
        FluteVoice& v = voices[i];

        if (v.voice_state.load(std::memory_order_relaxed) >= 0) {
            bool active_gate = v.gate || v.sustain_held;

            // Огибающая дыхания (Breath Envelope)
            if (active_gate) {
                v.breath_env += attack_rate;
                if (v.breath_env > 1.0f) v.breath_env = 1.0f;
            } else {
                v.breath_env -= release_rate;
                if (v.breath_env <= 0.0f) {
                    v.breath_env = 0.0f;
                    v.voice_state.store(-1, std::memory_order_release);
                    v.press_count = 0;
                    v.sustain_held = false;
                    v.gate = false;
                    continue;
                }
            }

            // LFO Вибрато
            v.vibrato_phase += vib_step;
            if (v.vibrato_phase >= 2.0f * 3.14159265f) {
                v.vibrato_phase -= 2.0f * 3.14159265f;
            }
            float vibrato = std::sin(v.vibrato_phase) * vib_depth;

            // Шум дыхания / турбулентность
            float noise = FastRandom(v.prng_state) * turb_gain;
            float breath_pressure = v.breath_env * (FLUTE_DEFAULT_BREATH + vibrato) + noise * v.breath_env;

            // Длина задержки трубки и струи
            float bore_len = v.bore_length;
            float jet_len = bore_len * jet_ratio;

            // Считывание отражённой волны из акустического резонатора
            float bore_out = v.filter_state;

            // Нелинейный возбудитель (Jet)
            float jet_in = breath_pressure - 0.5f * bore_out;
            v.jet_delay.Write(jet_in);
            float jet_delayed = v.jet_delay.Read(jet_len);
            float jet_out = JetTable(jet_delayed);

            // Акустический резонатор (Bore Delay Line)
            float bore_in = jet_out + 0.5f * bore_out;
            v.bore_delay.Write(bore_in);
            float bore_raw = v.bore_delay.Read(bore_len);

            // ФНЧ фильтрация затухания высоких частот о стенки трубки
            v.filter_state = v.filter_state + (1.0f - damping) * (bore_raw - v.filter_state);

            float voice_out = (0.35f * bore_raw + 0.35f * jet_out) * v.velocity_gain;

            active_voices_count++;
            mix_sum += voice_out;
        }
    }

    float norm_gain = 1.0f;
    if (active_voices_count > 1) {
        norm_gain = 1.0f / (1.0f + 0.35f * (float)(active_voices_count - 1));
    }

    float output_f = soft_limit(mix_sum * norm_gain * FLUTE_MASTER_GAIN);

    return (int16_t)(output_f * 32767.0f);
}

void FluteSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames) {
    for (uint32_t i = 0; i < num_frames; i++) {
        int16_t sample = FluteSynth_NextSample();
        buffer[i * 2]     = sample; // L
        buffer[i * 2 + 1] = sample; // R
    }
}

} // extern "C"

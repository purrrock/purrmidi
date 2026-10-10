#include "pluck_synth.h"
#include "daisysp.h"
#include "PhysicalModeling/pluck.h"
#include "Utility/dsp.h"
#include <atomic>
#include <cmath>
#include <cstring>

using namespace daisysp;

#define PLUCK_SAMPLE_RATE       48000.0f
#define PLUCK_BUFFER_SIZE       2048
#define PLUCK_RELEASE_TIME_SEC  0.25f   // Длительность работы гасителя (damper) после Note Off в секундах
#define PLUCK_MASTER_GAIN       0.85f   // Запас по громкости против клиппинга
#define PLUCK_MAX_DAMP          0.999f  // Потолок damp (выше 0.99 нужен для компенсации высоких нот)
#define EVENT_FIFO_SIZE         32      // Буфер событий NoteOn, NoteOff, CC (степень двойки)
#define PLUCK_OUTPUT_SCALE      (PLUCK_MASTER_GAIN * 32767.0f) // Предрасчитанная константа

enum PluckEventType : uint8_t {
    EVENT_NOTE_ON,
    EVENT_NOTE_OFF,
    EVENT_CONTROL_CHANGE
};

struct PluckEvent {
    PluckEventType type;
    uint8_t param1; // note number or CC control
    uint8_t param2; // velocity or CC value
    float freq;
    float amp;
    float decay;
    float damp_offset;
};

struct PluckVoice {
    Pluck string_voice;
    float pluck_buffer[PLUCK_BUFFER_SIZE + 16]; // Запас от выхода за границы при fp[npts_] в DaisySP
    std::atomic<int32_t> voice_state{-1};      // -1 = свободен, 0..127 = номер активной ноты
    float damp_offset{0.0f};
    float freq{440.0f};
    float damper_env{0.0f};
    float level_ema{0.0f};
    uint32_t trigger_age{0};
    uint8_t press_count{0};                    // Счётчик удержаний нажатия клавиши для данной ноты
    bool pending_trig{false};
    bool sustain_held{false};
};

// Буферы голосов размещаются в AXI SRAM (.dma_buffer), чтобы не переполнять DTCMRAM на STM32H7
#if defined(__GNUC__) && !defined(__EMSCRIPTEN__) && defined(__arm__)
static PluckVoice voices[PLUCK_VOICES] __attribute__((section(".dma_buffer"), aligned(32)));
#else
static PluckVoice voices[PLUCK_VOICES];
#endif

static uint32_t global_trigger_counter = 0;
static float release_coeff = 0.0f;

// Переменные состояния непрерывных контроллеров (MIDI CC)
static std::atomic<float> user_decay{0.96f};
static std::atomic<float> user_damp{0.85f};

static PluckEvent event_fifo[EVENT_FIFO_SIZE];
static std::atomic<uint32_t> fifo_head{0};
static std::atomic<uint32_t> fifo_tail{0};
static std::atomic<uint32_t> dropped_events_count{0};

static bool audio_sustain_pedal = false;

// Lock-free SPSC FIFO для событий MIDI
static bool event_fifo_push(const PluckEvent& event) {
    uint32_t head = fifo_head.load(std::memory_order_relaxed);
    uint32_t tail = fifo_tail.load(std::memory_order_acquire);

    if (head - tail >= EVENT_FIFO_SIZE) {
        dropped_events_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    event_fifo[head & (EVENT_FIFO_SIZE - 1)] = event;
    fifo_head.store(head + 1, std::memory_order_release);
    return true;
}

static bool event_fifo_pop(PluckEvent& event) {
    uint32_t tail = fifo_tail.load(std::memory_order_relaxed);
    uint32_t head = fifo_head.load(std::memory_order_acquire);

    if (head == tail) {
        return false;
    }

    event = event_fifo[tail & (EVENT_FIFO_SIZE - 1)];
    fifo_tail.store(tail + 1, std::memory_order_release);
    return true;
}

static inline float clamp_f(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static inline float compensate_damp(float damp, float freq) {
    const float ref_freq = 261.63f; // C4
    if (freq <= ref_freq) return damp;
    return clamp_f(1.0f - (1.0f - damp) * (ref_freq / freq), 0.0f, PLUCK_MAX_DAMP);
}

void PluckSynth_Init(void) {
    release_coeff = expf(-1.0f / (PLUCK_SAMPLE_RATE * PLUCK_RELEASE_TIME_SEC));
    global_trigger_counter = 0;
    audio_sustain_pedal = false;

    for (int i = 0; i < PLUCK_VOICES; i++) {
        std::memset(voices[i].pluck_buffer, 0, sizeof(voices[i].pluck_buffer));
        voices[i].string_voice.Init(PLUCK_SAMPLE_RATE, voices[i].pluck_buffer, PLUCK_BUFFER_SIZE - 1, PLUCK_MODE_RECURSIVE);
        voices[i].voice_state.store(-1, std::memory_order_relaxed);
        voices[i].damp_offset  = 0.0f;
        voices[i].freq         = 440.0f;
        voices[i].damper_env   = 0.0f;
        voices[i].level_ema    = 0.0f;
        voices[i].trigger_age  = 0;
        voices[i].pending_trig = false;
        voices[i].press_count  = 0;
        voices[i].sustain_held = false;

        voices[i].string_voice.SetFreq(440.0f);
        voices[i].string_voice.SetAmp(0.5f);
        voices[i].string_voice.SetDecay(0.96f);
        voices[i].string_voice.SetDamp(0.85f);
    }

    user_decay.store(0.96f, std::memory_order_relaxed);
    user_damp.store(0.85f, std::memory_order_relaxed);

    fifo_head.store(0, std::memory_order_relaxed);
    fifo_tail.store(0, std::memory_order_relaxed);
    dropped_events_count.store(0, std::memory_order_relaxed);
}

void PluckSynth_NoteOn(uint8_t midi_note, uint8_t velocity) {
    if (velocity == 0) {
        PluckSynth_NoteOff(midi_note);
        return;
    }

    if (midi_note >= 128) return;

    float freq = mtof((float)midi_note);
    float min_freq = (PLUCK_SAMPLE_RATE / (float)(PLUCK_BUFFER_SIZE - 4));
    float max_freq = (PLUCK_SAMPLE_RATE * 0.45f);
    freq = clamp_f(freq, min_freq, max_freq);

    float norm_vel = clamp_f((float)velocity * (1.0f / 127.0f), 0.0f, 1.0f);
    float amp = 0.05f + 0.95f * norm_vel;

    float damp_offset = norm_vel * 0.10f;
    float decay = user_decay.load(std::memory_order_relaxed);

    PluckEvent event;
    event.type        = EVENT_NOTE_ON;
    event.param1      = midi_note;
    event.param2      = velocity;
    event.freq        = freq;
    event.amp         = amp;
    event.decay       = decay;
    event.damp_offset = damp_offset;

    event_fifo_push(event);
}

void PluckSynth_NoteOff(uint8_t midi_note) {
    if (midi_note >= 128) return;

    PluckEvent event;
    event.type   = EVENT_NOTE_OFF;
    event.param1 = midi_note;
    event.param2 = 0;

    event_fifo_push(event);
}

void PluckSynth_SetDecay(float decay) {
    user_decay.store(clamp_f(decay, 0.0f, 1.0f), std::memory_order_relaxed);
}

void PluckSynth_SetDamp(float damp) {
    user_damp.store(clamp_f(damp, 0.0f, PLUCK_MAX_DAMP), std::memory_order_relaxed);
}

bool PluckSynth_IsNoteActive(uint8_t midi_note) {
    if (midi_note >= 128) return false;
    for (int i = 0; i < PLUCK_VOICES; i++) {
        if (voices[i].voice_state.load(std::memory_order_acquire) == (int32_t)midi_note) {
            return true;
        }
    }
    return false;
}

uint32_t PluckSynth_GetDroppedEventsCount(void) {
    return dropped_events_count.load(std::memory_order_relaxed);
}

void PluckSynth_ControlChange(uint8_t control, uint8_t value) {
    float norm = (float)value * (1.0f / 127.0f);

    switch (control) {
        case 1:  
        case 71: 
        case 74: 
            PluckSynth_SetDamp(0.30f + norm * 0.69f);
            break;

        case 72: 
            PluckSynth_SetDecay(0.50f + norm * 0.495f);
            break;

        case 64: {
            PluckEvent event;
            event.type   = EVENT_CONTROL_CHANGE;
            event.param1 = control;
            event.param2 = value;
            event_fifo_push(event);
            break;
        }

        default:
            break;
    }
}

int16_t PluckSynth_NextSample(void) {
    PluckEvent event;

    // 1. Последовательная строго хронологическая обработка событий из FIFO
    while (event_fifo_pop(event)) {
        if (event.type == EVENT_NOTE_ON) {
            int target_voice = -1;

            // Поиск голоса, уже играющего данную ноту
            for (int i = 0; i < PLUCK_VOICES; i++) {
                if (voices[i].voice_state.load(std::memory_order_relaxed) == (int32_t)event.param1) {
                    target_voice = i;
                    break;
                }
            }

            // Иначе ищем свободный голос
            if (target_voice == -1) {
                for (int i = 0; i < PLUCK_VOICES; i++) {
                    if (voices[i].voice_state.load(std::memory_order_relaxed) == -1) {
                        target_voice = i;
                        break;
                    }
                }
            }

            // Иначе вытесняем самый старый голос
            if (target_voice == -1) {
                uint32_t max_age_diff = 0;
                target_voice = 0;
                for (int i = 0; i < PLUCK_VOICES; i++) {
                    uint32_t age_diff = global_trigger_counter - voices[i].trigger_age;
                    if (age_diff >= max_age_diff) {
                        max_age_diff = age_diff;
                        target_voice = i;
                    }
                }
            }

            PluckVoice& v = voices[target_voice];
            v.freq         = event.freq;
            v.damp_offset  = event.damp_offset;
            v.damper_env   = 1.0f;
            v.level_ema    = 1.0f;
            v.trigger_age  = ++global_trigger_counter;
            v.pending_trig = true;
            if (v.press_count < 255) v.press_count++;
            v.sustain_held = false;
            v.voice_state.store((int32_t)event.param1, std::memory_order_release);

            v.string_voice.SetFreq(event.freq);
            v.string_voice.SetAmp(event.amp);
            v.string_voice.SetDecay(event.decay);

            float current_damp = user_damp.load(std::memory_order_relaxed);
            v.string_voice.SetDamp(compensate_damp(
                clamp_f(current_damp + v.damp_offset, 0.0f, 1.0f), v.freq));

        } else if (event.type == EVENT_NOTE_OFF) {
            uint8_t note = event.param1;
            for (int i = 0; i < PLUCK_VOICES; i++) {
                if (voices[i].voice_state.load(std::memory_order_relaxed) == (int32_t)note && voices[i].press_count > 0) {
                    voices[i].press_count--;
                    if (voices[i].press_count == 0) {
                        if (audio_sustain_pedal) {
                            voices[i].sustain_held = true; // Захват педалью ТОЛЬКО если педаль была нажата
                        } else {
                            voices[i].sustain_held = false;
                        }
                    }
                }
            }
        } else if (event.type == EVENT_CONTROL_CHANGE) {
            if (event.param1 == 64) {
                bool pedal_on = (event.param2 >= 64);
                audio_sustain_pedal = pedal_on;
                if (!pedal_on) {
                    // Отпускание педали снимает удержание у всех голосов
                    for (int i = 0; i < PLUCK_VOICES; i++) {
                        voices[i].sustain_held = false;
                    }
                }
            }
        }
    }

    // 2. Вычисление и смешивание сигналов активных голосов
    float mix_sum_f = 0.0f;
    uint32_t active_voices_count = 0;
    float current_decay = user_decay.load(std::memory_order_relaxed);
    float current_damp = user_damp.load(std::memory_order_relaxed);

    for (int i = 0; i < PLUCK_VOICES; i++) {
        PluckVoice& v = voices[i];

        if (v.voice_state.load(std::memory_order_relaxed) >= 0) {
            v.string_voice.SetDecay(current_decay);
            v.string_voice.SetDamp(compensate_damp(
                clamp_f(current_damp + v.damp_offset, 0.0f, 1.0f), v.freq));

            float trig = v.pending_trig ? 1.0f : 0.0f;
            v.pending_trig = false;

            float sample_f = v.string_voice.Process(trig);

            // Если клавиша отпущена И нота не удерживается педалью CC64, опускаем демпфер (damper decay)
            if (v.press_count == 0 && !v.sustain_held) {
                v.damper_env *= release_coeff;
            }

            sample_f *= v.damper_env;

            // Оценка сглаженного уровня энергии (EMA)
            v.level_ema = 0.99f * v.level_ema + 0.01f * std::abs(sample_f);

            if (v.level_ema < 0.0001f) {
                v.voice_state.store(-1, std::memory_order_release);
                v.press_count  = 0;
                v.sustain_held = false;
                sample_f       = 0.0f;
            } else {
                active_voices_count++;
                mix_sum_f += sample_f;
            }
        }
    }

    // Динамическая нормализация при суммировании нескольких голосов
    float norm_gain = 1.0f;
    if (active_voices_count > 1) {
        norm_gain = 1.0f / (1.0f + 0.40f * (float)(active_voices_count - 1));
    }

    float output_sample_f = mix_sum_f * norm_gain * PLUCK_OUTPUT_SCALE;

    if (output_sample_f > 32767.0f) {
        output_sample_f = 32767.0f;
    } else if (output_sample_f < -32768.0f) {
        output_sample_f = -32768.0f;
    }

    return (int16_t)output_sample_f;
}

void PluckSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames) {
    for (uint32_t i = 0; i < num_frames; i++) {
        int16_t sample = PluckSynth_NextSample();
        buffer[i * 2]     = sample; // L
        buffer[i * 2 + 1] = sample; // R
    }
}
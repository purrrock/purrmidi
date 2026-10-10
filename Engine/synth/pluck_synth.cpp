#include "pluck_synth.h"
#include "daisysp.h"
#include "PhysicalModeling/pluck.h"
#include "Utility/dsp.h"
#include <atomic>
#include <cmath>

using namespace daisysp;

#define PLUCK_SAMPLE_RATE       48000.0f
#define PLUCK_BUFFER_SIZE       2048
#define PLUCK_RELEASE_TIME_SEC  0.25f   // Длительность работы гасителя (damper) после Note Off в секундах
#define PLUCK_MASTER_GAIN       0.85f   // Запас по громкости против клиппинга
#define PLUCK_MAX_DAMP          0.999f  // Потолок damp (выше 0.99 нужен для компенсации высоких нот)
#define NOTE_EVENT_FIFO_SIZE    16      // Размер FIFO буфера событий NoteOn (должен быть степенью двойки)
#define PLUCK_OUTPUT_SCALE      (PLUCK_MASTER_GAIN * 32767.0f) // Предрасчитанная константа

struct NoteOnEvent {
    uint8_t note;
    float freq;
    float amp;
    float decay;
    float damp_offset; // Сохраняем только прибавку от velocity, а не финальное значение
};

struct PluckVoice {
    Pluck string_voice;
    float pluck_buffer[PLUCK_BUFFER_SIZE];
    int16_t midi_note;
    float damp_offset;
    float freq;
    float damper_env;
    float level_ema;
    uint32_t trigger_age;
    bool pending_trig;
    bool active;
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

static NoteOnEvent note_event_fifo[NOTE_EVENT_FIFO_SIZE];
static std::atomic<uint32_t> fifo_head{0};
static std::atomic<uint32_t> fifo_tail{0};
static std::atomic<uint32_t> dropped_events_count{0};

static std::atomic<bool> sustain_pedal{false};
static std::atomic<bool> note_pressed[128];

// Lock-free SPSC (Single-Producer Single-Consumer) FIFO для событий NoteOn
static bool note_event_fifo_push(const NoteOnEvent& event) {
    uint32_t head = fifo_head.load(std::memory_order_relaxed);
    uint32_t tail = fifo_tail.load(std::memory_order_acquire);

    if (head - tail >= NOTE_EVENT_FIFO_SIZE) {
        dropped_events_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    note_event_fifo[head & (NOTE_EVENT_FIFO_SIZE - 1)] = event;
    fifo_head.store(head + 1, std::memory_order_release);
    return true;
}

static bool note_event_fifo_pop(NoteOnEvent& event) {
    uint32_t tail = fifo_tail.load(std::memory_order_relaxed);
    uint32_t head = fifo_head.load(std::memory_order_acquire);

    if (head == tail) {
        return false;
    }

    event = note_event_fifo[tail & (NOTE_EVENT_FIFO_SIZE - 1)];
    fifo_tail.store(tail + 1, std::memory_order_release);
    return true;
}

// Вспомогательная функция ограничения диапазона
static inline float clamp_f(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

// Фильтр Pluck срабатывает один раз за проход по буферу, т.е. freq раз в секунду,
// а потери за проход пропорциональны (1 - damp). Выше ref_freq уменьшаем (1 - damp)
// обратно пропорционально частоте, чтобы время звучания не зависело от высоты ноты.
static inline float compensate_damp(float damp, float freq) {
    const float ref_freq = 261.63f; // C4 - ниже этой частоты ничего не меняем
    if (freq <= ref_freq) return damp;
    return clamp_f(1.0f - (1.0f - damp) * (ref_freq / freq), 0.0f, PLUCK_MAX_DAMP);
}

void PluckSynth_Init(void) {
    release_coeff = expf(-1.0f / (PLUCK_SAMPLE_RATE * PLUCK_RELEASE_TIME_SEC));
    global_trigger_counter = 0;

    for (int i = 0; i < PLUCK_VOICES; i++) {
        voices[i].string_voice.Init(PLUCK_SAMPLE_RATE, voices[i].pluck_buffer, PLUCK_BUFFER_SIZE, PLUCK_MODE_RECURSIVE);
        voices[i].midi_note    = -1;
        voices[i].damp_offset  = 0.0f;
        voices[i].freq         = 440.0f;
        voices[i].damper_env   = 0.0f;
        voices[i].level_ema    = 0.0f;
        voices[i].trigger_age  = 0;
        voices[i].pending_trig = false;
        voices[i].active       = false;

        voices[i].string_voice.SetFreq(440.0f);
        voices[i].string_voice.SetAmp(0.5f);
        voices[i].string_voice.SetDecay(0.96f);
        voices[i].string_voice.SetDamp(0.85f);
    }

    user_decay.store(0.96f, std::memory_order_relaxed);
    user_damp.store(0.85f, std::memory_order_relaxed);

    sustain_pedal.store(false, std::memory_order_relaxed);
    for (int i = 0; i < 128; i++) {
        note_pressed[i].store(false, std::memory_order_relaxed);
    }
    
    fifo_head.store(0, std::memory_order_relaxed);
    fifo_tail.store(0, std::memory_order_relaxed);
    dropped_events_count.store(0, std::memory_order_relaxed);
}

void PluckSynth_NoteOn(uint8_t midi_note, uint8_t velocity) {
    if (velocity == 0) {
        PluckSynth_NoteOff(midi_note);
        return;
    }

    if (midi_note < 128) {
        note_pressed[midi_note].store(true, std::memory_order_relaxed);
    }

    float freq = mtof((float)midi_note);
    float min_freq = (PLUCK_SAMPLE_RATE / (float)(PLUCK_BUFFER_SIZE - 4));
    float max_freq = (PLUCK_SAMPLE_RATE * 0.45f);
    freq = clamp_f(freq, min_freq, max_freq);

    float norm_vel = clamp_f((float)velocity * (1.0f / 127.0f), 0.0f, 1.0f);
    float amp = 0.05f + 0.95f * norm_vel;

    // Рассчитываем только смещение от динамики удара, чтобы не затирать ручку Damp
    float damp_offset = norm_vel * 0.10f;
    
    // Берем актуальный decay для отправки в событие
    float decay = user_decay.load(std::memory_order_relaxed);

    NoteOnEvent event;
    event.note        = midi_note;
    event.freq        = freq;
    event.amp         = amp;
    event.decay       = decay;
    event.damp_offset = damp_offset;

    note_event_fifo_push(event);
}

void PluckSynth_NoteOff(uint8_t midi_note) {
    if (midi_note < 128) {
        note_pressed[midi_note].store(false, std::memory_order_relaxed);
    }
}

void PluckSynth_SetDecay(float decay) {
    user_decay.store(clamp_f(decay, 0.0f, 1.0f), std::memory_order_relaxed);
}

void PluckSynth_SetDamp(float damp) {
    user_damp.store(clamp_f(damp, 0.0f, PLUCK_MAX_DAMP), std::memory_order_relaxed);
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

        case 64: 
            sustain_pedal.store(value >= 64, std::memory_order_relaxed);
            break;

        default:
            break;
    }
}

int16_t PluckSynth_NextSample(void) {
    NoteOnEvent event;

    // 1. Проверяем наличие новых событий нот и выделяем голоса
    while (note_event_fifo_pop(event)) {
        int target_voice = -1;

        // Поиск активного голоса, уже играющего данную ноту
        for (int i = 0; i < PLUCK_VOICES; i++) {
            if (voices[i].active && voices[i].midi_note == (int16_t)event.note) {
                target_voice = i;
                break;
            }
        }

        // Если не найден, ищем свободный/неактивный голос
        if (target_voice == -1) {
            for (int i = 0; i < PLUCK_VOICES; i++) {
                if (!voices[i].active || voices[i].midi_note == -1) {
                    target_voice = i;
                    break;
                }
            }
        }

        // Если все голоса заняты, вытесняем самый старый активный голос
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
        v.active       = true;
        v.midi_note    = (int16_t)event.note;
        v.freq         = event.freq;
        v.damp_offset  = event.damp_offset;
        v.damper_env   = 1.0f;
        v.level_ema    = 1.0f;
        v.trigger_age  = ++global_trigger_counter;
        v.pending_trig = true;

        v.string_voice.SetFreq(event.freq);
        v.string_voice.SetAmp(event.amp);
        v.string_voice.SetDecay(event.decay);

        float current_damp = user_damp.load(std::memory_order_relaxed);
        v.string_voice.SetDamp(compensate_damp(
            clamp_f(current_damp + v.damp_offset, 0.0f, 1.0f), v.freq));
    }

    // 2. Вычисление и смешивание сигналов активных голосов
    float mix_sum_f = 0.0f;
    uint32_t active_voices_count = 0;
    float current_decay = user_decay.load(std::memory_order_relaxed);
    float current_damp = user_damp.load(std::memory_order_relaxed);

    for (int i = 0; i < PLUCK_VOICES; i++) {
        PluckVoice& v = voices[i];

        if (v.active) {
            v.string_voice.SetDecay(current_decay);
            v.string_voice.SetDamp(compensate_damp(
                clamp_f(current_damp + v.damp_offset, 0.0f, 1.0f), v.freq));

            float trig = v.pending_trig ? 1.0f : 0.0f;
            v.pending_trig = false;

            float sample_f = v.string_voice.Process(trig);

            // Работа гасителя (damper): если клавиша отпущена и педаль Sustain не нажата,
            // прижимаем струну демпфером (ускоренное гашение). Если педаль нажата или клавиша удерживается,
            // струна затухает естественно по алгоритму Karplus-Strong.
            if (v.midi_note >= 0 && v.midi_note < 128) {
                bool is_pressed = note_pressed[v.midi_note].load(std::memory_order_relaxed);
                bool sus_pedal  = sustain_pedal.load(std::memory_order_relaxed);

                if (!is_pressed && !sus_pedal) {
                    v.damper_env *= release_coeff;
                }
            }

            sample_f *= v.damper_env;

            // Оценка сглаженного уровня энергии (EMA) для устойчивого освобождения голосa
            v.level_ema = 0.99f * v.level_ema + 0.01f * std::abs(sample_f);

            if (v.level_ema < 0.0001f) {
                v.active = false;
                v.midi_note = -1;
                sample_f = 0.0f;
            } else {
                active_voices_count++;
                mix_sum_f += sample_f;
            }
        }
    }

    // Динамическая нормализация при суммировании нескольких голосов для сохранения динамики
    float norm_gain = 1.0f;
    if (active_voices_count > 1) {
        norm_gain = 1.0f / (1.0f + 0.25f * (float)(active_voices_count - 1));
    }

    // 3. Масштабирование с учетом гейна и зажимание уровня в int16_t
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
        buffer[i * 2]     = sample; // Левый канал (L)
        buffer[i * 2 + 1] = sample; // Правый канал (R)
    }
}
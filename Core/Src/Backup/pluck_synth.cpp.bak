#include "pluck_synth.h"
#include "daisysp.h"
#include "PhysicalModeling/pluck.h"
#include "Utility/dsp.h"
#include <atomic>
#include <cmath>

using namespace daisysp;

#define PLUCK_SAMPLE_RATE       48000.0f
#define PLUCK_BUFFER_SIZE       2048
#define PLUCK_RELEASE_TIME_SEC  0.25f   // Длительность release огибающей в секундах
#define PLUCK_MASTER_GAIN       0.85f   // Запас по громкости против клиппинга
#define PLUCK_MAX_DAMP          0.999f  // Потолок damp (выше 0.99 нужен для компенсации высоких нот)
#define NOTE_EVENT_FIFO_SIZE    16      // Размер FIFO буфера событий NoteOn (должен быть степенью двойки)
#define PLUCK_OUTPUT_SCALE      (PLUCK_MASTER_GAIN * 32767.0f) // Предрасчитанная константа[cite: 9]

struct NoteOnEvent {
    uint8_t note;
    float freq;
    float amp;
    float decay;
    float damp_offset; // Сохраняем только прибавку от velocity, а не финальное значение
};

// Объект физического моделирования струны и буфер линии задержки (8 КБ)[cite: 8]
static Pluck string_voice;
static float pluck_buffer[PLUCK_BUFFER_SIZE];

// Переменные огибающей и состояния в контексте аудиопотока
static float release_coeff = 0.0f;
static float envelope = 0.0f;
static int16_t active_audio_note = -1;
static float active_damp_offset = 0.0f; // Удерживает влияние удара по клавише на время звучания ноты
static float active_freq = 440.0f;      // Частота активной ноты (для компенсации damp)

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
    // Инициализация алгоритма Карплуса-Стронга в рекурсивном режиме сглаживания
    string_voice.Init(PLUCK_SAMPLE_RATE, pluck_buffer, PLUCK_BUFFER_SIZE, PLUCK_MODE_RECURSIVE);

    release_coeff      = expf(-1.0f / (PLUCK_SAMPLE_RATE * PLUCK_RELEASE_TIME_SEC));
    envelope           = 0.0f;
    active_audio_note  = -1;
    active_damp_offset = 0.0f;
    active_freq        = 440.0f;

    user_decay.store(0.96f, std::memory_order_relaxed);
    user_damp.store(0.85f, std::memory_order_relaxed);

    sustain_pedal.store(false, std::memory_order_relaxed);
    for (int i = 0; i < 128; i++) {
        note_pressed[i].store(false, std::memory_order_relaxed);
    }
    
    fifo_head.store(0, std::memory_order_relaxed);
    fifo_tail.store(0, std::memory_order_relaxed);
    dropped_events_count.store(0, std::memory_order_relaxed);

    string_voice.SetFreq(440.0f);
    string_voice.SetAmp(0.5f);
    string_voice.SetDecay(0.96f);
    string_voice.SetDamp(0.85f);
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
    float trig = 0.0f;
    NoteOnEvent event;

    // 1. Проверяем наличие новых событий нот
    if (note_event_fifo_pop(event)) {
        string_voice.SetFreq(event.freq);
        string_voice.SetAmp(event.amp);
        string_voice.SetDecay(event.decay);
        
        // Фиксируем смещение damp от текущего удара по клавише
        active_damp_offset = event.damp_offset;
        active_freq        = event.freq;
        float current_damp = user_damp.load(std::memory_order_relaxed);
        string_voice.SetDamp(compensate_damp(
            clamp_f(current_damp + active_damp_offset, 0.0f, 1.0f), active_freq));

        trig = 1.0f;
        envelope = 1.0f;
        active_audio_note = (int16_t)event.note;
    } else {
        // 2. Если новой ноты нет, непрерывно применяем положение ручек
        string_voice.SetDecay(user_decay.load(std::memory_order_relaxed));
        
        // Добавляем к глобальному Damp сохраненное смещение активной ноты
        float current_damp = user_damp.load(std::memory_order_relaxed);
        string_voice.SetDamp(compensate_damp(
            clamp_f(current_damp + active_damp_offset, 0.0f, 1.0f), active_freq));

        trig = 0.0f;
    }

    float sample_f = 0.0f;

    // 3. Вычисление физического моделирования с экономией CPU в моменты тишины[cite: 9]
    if (envelope > 0.0f) {
        sample_f = string_voice.Process(trig);

        if (active_audio_note >= 0 && active_audio_note < 128) {
            bool is_pressed = note_pressed[active_audio_note].load(std::memory_order_relaxed);
            bool sus_pedal  = sustain_pedal.load(std::memory_order_relaxed);

            // Если клавиша отпущена и педаль не нажата, запускаем затухание огибающей
            if (!is_pressed && !sus_pedal) {
                envelope *= release_coeff;
                if (envelope < 0.0001f) {
                    envelope = 0.0f;
                    active_audio_note = -1;
                }
            }
        }

        sample_f *= envelope;
        sample_f *= PLUCK_OUTPUT_SCALE; // Применение предрасчитанной константы гейна[cite: 9]

        // Жёсткое ограничение для защиты от переполнения int16_t (Hard Clipping)
        if (sample_f > 32767.0f) {
            sample_f = 32767.0f;
        } else if (sample_f < -32768.0f) {
            sample_f = -32768.0f;
        }
    }

    return (int16_t)sample_f;
}

void PluckSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames) {
    for (uint32_t i = 0; i < num_frames; i++) {
        int16_t sample = PluckSynth_NextSample();
        buffer[i * 2]     = sample; // Левый канал (L)
        buffer[i * 2 + 1] = sample; // Правый канал (R)
    }
}
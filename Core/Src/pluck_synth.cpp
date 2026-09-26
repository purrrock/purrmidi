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

#define NOTE_EVENT_FIFO_SIZE    16      // Размер FIFO буфера событий NoteOn (должен быть степенью двойки)

struct NoteOnEvent {
    uint8_t note;
    float freq;
    float amp;
    float damp;
    float decay;
};

// Объект физического моделирования струны и буфер линии задержки (8 КБ)
static Pluck string_voice;
static float pluck_buffer[PLUCK_BUFFER_SIZE];

// Переменные огибающей release (контекст аудиопотока)
static float release_coeff = 0.0f;
static float envelope = 0.0f;
static int16_t active_audio_note = -1;

struct SynthParams {
    float freq;
    float amp;
    float decay;
    float damp;
};

// Переменные состояния в контексте управления (MIDI / control)
static float user_decay = 0.96f;
static float user_damp  = 0.85f;
static float current_control_freq  = 440.0f;
static float current_control_amp   = 0.5f;
static float current_control_decay = 0.96f;
static float current_control_damp  = 0.85f;

#define PARAMS_FIFO_SIZE    16      // Размер FIFO буфера параметров (должен быть степенью двойки)
#define PARAMS_FIFO_MASK    (PARAMS_FIFO_SIZE - 1)

// Lock-Free Bounded SPSC FIFO для передачи полных снимков SynthParams из контекста управления в аудиоконтекст.
// Владение слотами (Slot Ownership Model):
// - Слоты в диапазоне [params_tail, params_head - 1] принадлежат Consumer (Audio context) и зафиксированы от перезаписи.
// - Слоты вне этого диапазона свободны и принадлежат Producer (Control context).
// - Producer проверяет условие (head - tail < PARAMS_FIFO_SIZE). Если FIFO заполнено, Producer отбрасывает пуш,
//   гарантируя, что слоты Consumer никогда не будут перезаписаны во время чтения или ожидания.
static SynthParams params_fifo[PARAMS_FIFO_SIZE];
static std::atomic<uint32_t> params_head{0};
static std::atomic<uint32_t> params_tail{0};

static NoteOnEvent note_event_fifo[NOTE_EVENT_FIFO_SIZE];
static std::atomic<uint32_t> fifo_head{0};
static std::atomic<uint32_t> fifo_tail{0};
static volatile uint32_t dropped_events_count = 0;

static std::atomic<bool> sustain_pedal{false};
static std::atomic<int16_t> current_note{-1};
static std::atomic<bool> note_pressed[128];

// Вспомогательная функция публикации снимка параметров из контекста управления
static void CommitParams(float freq, float amp, float decay, float damp) {
    current_control_freq  = freq;
    current_control_amp   = amp;
    current_control_decay = decay;
    current_control_damp  = damp;

    uint32_t head = params_head.load(std::memory_order_relaxed);
    uint32_t tail = params_tail.load(std::memory_order_acquire);

    // Producer пишет только в гарантированно свободный слот head, если FIFO не заполнено
    if (head - tail >= PARAMS_FIFO_SIZE) {
        return;
    }

    params_fifo[head & PARAMS_FIFO_MASK] = { freq, amp, decay, damp };
    params_head.store(head + 1, std::memory_order_release);
}

// Вспомогательные функции lock-free SPSC FIFO для событий NoteOn
static bool note_event_fifo_push(const NoteOnEvent& event) {
    uint32_t head = fifo_head.load(std::memory_order_relaxed);
    uint32_t tail = fifo_tail.load(std::memory_order_acquire);

    if (head - tail >= NOTE_EVENT_FIFO_SIZE) {
        dropped_events_count++;
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

// Вспомогательная функция ограничения диапазона float
static inline float clamp_f(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

void PluckSynth_Init(void) {
    // Инициализация алгоритма Карплуса — Стронга в рекурсивном режиме сглаживания
    string_voice.Init(PLUCK_SAMPLE_RATE, pluck_buffer, PLUCK_BUFFER_SIZE, PLUCK_MODE_RECURSIVE);

    release_coeff     = expf(-1.0f / (PLUCK_SAMPLE_RATE * PLUCK_RELEASE_TIME_SEC));
    envelope          = 0.0f;
    active_audio_note = -1;

    user_decay = 0.96f;
    user_damp  = 0.85f;
    current_control_freq  = 440.0f;
    current_control_amp   = 0.5f;
    current_control_decay = 0.96f;
    current_control_damp  = 0.85f;

    for (uint32_t i = 0; i < PARAMS_FIFO_SIZE; i++) {
        params_fifo[i] = { 440.0f, 0.5f, 0.96f, 0.85f };
    }
    params_head.store(0, std::memory_order_relaxed);
    params_tail.store(0, std::memory_order_relaxed);

    sustain_pedal.store(false, std::memory_order_relaxed);
    current_note.store(-1, std::memory_order_relaxed);
    for (int i = 0; i < 128; i++) {
        note_pressed[i].store(false, std::memory_order_relaxed);
    }
    fifo_head.store(0, std::memory_order_relaxed);
    fifo_tail.store(0, std::memory_order_relaxed);
    dropped_events_count = 0;

    string_voice.SetFreq(440.0f);
    string_voice.SetAmp(0.5f);
    string_voice.SetDecay(0.96f);
    string_voice.SetDamp(0.85f);
}

void PluckSynth_NoteOn(uint8_t midi_note, uint8_t velocity) {
    // По стандарту MIDI сообщение Note On с velocity == 0 эквивалентно Note Off
    if (velocity == 0) {
        PluckSynth_NoteOff(midi_note);
        return;
    }

    if (midi_note < 128) {
        note_pressed[midi_note].store(true, std::memory_order_relaxed);
    }
    current_note.store((int16_t)midi_note, std::memory_order_relaxed);

    // Перевод номера MIDI-ноты в частоту в герцах (функция mtof из DaisySP)
    float freq = mtof((float)midi_note);
    // Защита от выхода за пределы буфера линии задержки и частоты Найквиста
    float min_freq = (PLUCK_SAMPLE_RATE / (float)(PLUCK_BUFFER_SIZE - 4));
    float max_freq = (PLUCK_SAMPLE_RATE * 0.45f);
    freq = clamp_f(freq, min_freq, max_freq);

    // Чувствительность к силе нажатия (Velocity -> Амплитуда 0.05 .. 1.0)
    float norm_vel = clamp_f((float)velocity * (1.0f / 127.0f), 0.0f, 1.0f);
    float amp = 0.05f + 0.95f * norm_vel;

    // Чем сильнее удар по клавише, тем ярче звучит струна при щипке
    float dynamic_damp = user_damp + (norm_vel * 0.10f);
    float damp = clamp_f(dynamic_damp, 0.0f, 0.99f);

    float decay = user_decay;

    // Публикуем обновленный snapshot параметров через lock-free механизм
    CommitParams(freq, amp, decay, damp);

    NoteOnEvent event;
    event.note  = midi_note;
    event.freq  = freq;
    event.amp   = amp;
    event.damp  = damp;
    event.decay = decay;

    note_event_fifo_push(event);
}

void PluckSynth_NoteOff(uint8_t midi_note) {
    if (midi_note < 128) {
        note_pressed[midi_note].store(false, std::memory_order_relaxed);
    }
    int16_t expected = (int16_t)midi_note;
    current_note.compare_exchange_strong(expected, -1, std::memory_order_relaxed);
}

void PluckSynth_SetDecay(float decay) {
    user_decay = clamp_f(decay, 0.0f, 1.0f);
    CommitParams(current_control_freq, current_control_amp, user_decay, current_control_damp);
}

void PluckSynth_SetDamp(float damp) {
    user_damp = clamp_f(damp, 0.0f, 1.0f);
    float clamped_damp = clamp_f(user_damp, 0.0f, 0.99f);
    CommitParams(current_control_freq, current_control_amp, current_control_decay, clamped_damp);
}

void PluckSynth_ControlChange(uint8_t control, uint8_t value) {
    float norm = (float)value * (1.0f / 127.0f);

    switch (control) {
        case 1:  // Modulation Wheel (CC 1) -> Яркость / приглушение струны (Damp)
        case 71: // Timbre / Resonance (CC 71)
            PluckSynth_SetDamp(0.30f + norm * 0.69f);
            break;

case 72: // Release Time (CC 72) -> Длительность звучания струны (Decay)
    PluckSynth_SetDecay(0.50f + norm * 0.495f);
    break;

case 74: // Brightness / Cutoff (CC 74) -> Damp
    PluckSynth_SetDamp(0.30f + norm * 0.69f);
    break;

        case 64: // Sustain Pedal (CC 64)
            sustain_pedal.store(value >= 64, std::memory_order_relaxed);
            break;

        default:
            break;
    }
}

int16_t PluckSynth_NextSample(void) {
    float trig = 0.0f;
    NoteOnEvent event;

    if (note_event_fifo_pop(event)) {
        string_voice.SetFreq(event.freq);
        string_voice.SetAmp(event.amp);
        string_voice.SetDecay(event.decay);
        string_voice.SetDamp(event.damp);
        trig = 1.0f;
        envelope = 1.0f;
        active_audio_note = (int16_t)event.note;
    } else {
        // Проверяем наличие новых снимков параметров в SPSC FIFO
        uint32_t head = params_head.load(std::memory_order_acquire);
        uint32_t tail = params_tail.load(std::memory_order_relaxed);

        if (head != tail) {
            // Consumer забирает самый свежий snapshot из (head - 1) & PARAMS_FIFO_MASK
            const SynthParams& snapshot = params_fifo[(head - 1) & PARAMS_FIFO_MASK];
            string_voice.SetFreq(snapshot.freq);
            string_voice.SetAmp(snapshot.amp);
            string_voice.SetDecay(snapshot.decay);
            string_voice.SetDamp(snapshot.damp);

            // Продвигаем tail сразу к head, за раз освобождая все накопленные слоты
            params_tail.store(head, std::memory_order_release);
        }
        trig = 0.0f;
    }

    // Вычисляем отсчёт физического моделирования струны (-1.0f .. +1.0f)
    float sample_f = string_voice.Process(trig);

    if (active_audio_note >= 0 && active_audio_note < 128) {
        bool is_pressed = note_pressed[active_audio_note].load(std::memory_order_relaxed);
        bool sus_pedal  = sustain_pedal.load(std::memory_order_relaxed);

        if (!is_pressed && !sus_pedal) {
            envelope *= release_coeff;
            if (envelope < 0.0001f) {
                envelope = 0.0f;
                active_audio_note = -1;
            }
        }
    }

    sample_f *= envelope;
    sample_f *= (PLUCK_MASTER_GAIN * 32767.0f);

    // Жёсткое ограничение (Clamping) для защиты от переполнения int16_t
    if (sample_f > 32767.0f) {
        sample_f = 32767.0f;
    } else if (sample_f < -32768.0f) {
        sample_f = -32768.0f;
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

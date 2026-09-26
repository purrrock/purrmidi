#include "pluck_synth.h"
#include "daisysp.h"
#include "PhysicalModeling/pluck.h"
#include "Utility/dsp.h"
using namespace daisysp;

#define PLUCK_SAMPLE_RATE       48000.0f
#define PLUCK_BUFFER_SIZE       2048
#define PLUCK_RELEASE_DECAY     0.72f   // Ускоренное затухание при отпускании клавиши
#define PLUCK_MASTER_GAIN       0.85f   // Запас по громкости против клиппинга

// Объект физического моделирования струны и буфер линии задержки (8 КБ)
static Pluck string_voice;
static float pluck_buffer[PLUCK_BUFFER_SIZE];

// Переменные синхронизации между main() и прерыванием аудио-DMA
static volatile float pending_freq    = 440.0f;
static volatile float pending_amp     = 0.5f;
static volatile float user_decay      = 0.96f;
static volatile float active_decay    = 0.96f;
static volatile float user_damp       = 0.85f;
static volatile float pending_damp    = 0.85f;

static volatile bool trigger_pending  = false;
static volatile uint32_t params_sequence = 0;
static uint32_t applied_sequence      = 0;
static volatile bool sustain_pedal    = false;
static volatile int16_t current_note  = -1;

// Вспомогательная функция ограничения диапазона float
static inline float clamp_f(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

void PluckSynth_Init(void) {
    // Инициализация алгоритма Карплуса — Стронга в рекурсивном режиме сглаживания
    string_voice.Init(PLUCK_SAMPLE_RATE, pluck_buffer, PLUCK_BUFFER_SIZE, PLUCK_MODE_RECURSIVE);

    pending_freq    = 440.0f;
    pending_amp     = 0.5f;
    user_decay      = 0.96f;
    active_decay    = 0.96f;
    user_damp       = 0.85f;
    pending_damp    = 0.85f;
    sustain_pedal   = false;
    current_note    = -1;
    trigger_pending = false;
    params_sequence  = 0;
    applied_sequence = 0;

    string_voice.SetFreq(pending_freq);
    string_voice.SetAmp(pending_amp);
    string_voice.SetDecay(active_decay);
    string_voice.SetDamp(pending_damp);
}

void PluckSynth_NoteOn(uint8_t midi_note, uint8_t velocity) {
    // По стандарту MIDI сообщение Note On с velocity == 0 эквивалентно Note Off
    if (velocity == 0) {
        PluckSynth_NoteOff(midi_note);
        return;
    }

    current_note = (int16_t)midi_note;

    // Перевод номера MIDI-ноты в частоту в герцах (функция mtof из DaisySP)
    float freq = mtof((float)midi_note);
    // Защита от выхода за пределы буфера линии задержки и частоты Найквиста
    float min_freq = (PLUCK_SAMPLE_RATE / (float)(PLUCK_BUFFER_SIZE - 4));
    float max_freq = (PLUCK_SAMPLE_RATE * 0.45f);
    pending_freq = clamp_f(freq, min_freq, max_freq);

    // Чувствительность к силе нажатия (Velocity -> Амплитуда 0.05 .. 1.0)
    float norm_vel = (float)velocity * (1.0f / 127.0f);
    pending_amp = 0.05f + 0.95f * norm_vel;

    // Чем сильнее удар по клавише, тем ярче звучит струна при щипке
    float dynamic_damp = user_damp + (norm_vel * 0.10f);
    pending_damp = clamp_f(dynamic_damp, 0.0f, 0.99f);

    // Восстанавливаем рабочее время затухания струны и взводим флаг щипка
    active_decay    = user_decay;
    params_sequence++;
    trigger_pending = true;
}

void PluckSynth_NoteOff(uint8_t midi_note) {
    // Глушим струну только если отпущена именно та нота, которая сейчас звучит
    if (current_note == (int16_t)midi_note) {
        current_note = -1;
        if (!sustain_pedal) {
            // Плавное приглушение струны вместо резкого сброса в 0 (без щелчка)
            active_decay = (user_decay < PLUCK_RELEASE_DECAY) ? user_decay : PLUCK_RELEASE_DECAY;
            params_sequence++;
        }
    }
}

void PluckSynth_SetDecay(float decay) {
    user_decay   = clamp_f(decay, 0.0f, 1.0f);
    active_decay = user_decay;
    params_sequence++;
}

void PluckSynth_SetDamp(float damp) {
    user_damp    = clamp_f(damp, 0.0f, 1.0f);
    pending_damp = clamp_f(user_damp, 0.0f, 0.99f);
    params_sequence++;
}

void PluckSynth_ControlChange(uint8_t control, uint8_t value) {
    float norm = (float)value * (1.0f / 127.0f);

    switch (control) {
        case 1:  // Modulation Wheel (CC 1) -> Яркость / приглушение струны (Damp)
        case 71: // Timbre / Resonance (CC 71)
            PluckSynth_SetDamp(0.30f + norm * 0.69f);
            break;

        case 72: // Release Time (CC 72) -> Длительность звучания струны (Decay)
        case 74: // Brightness / Cutoff (CC 74)
            PluckSynth_SetDecay(0.50f + norm * 0.495f);
            break;

        case 64: // Sustain Pedal (CC 64)
            sustain_pedal = (value >= 64);
            if (!sustain_pedal && current_note < 0) {
                active_decay = PLUCK_RELEASE_DECAY;
                params_sequence++;
            }
            break;

        default:
            break;
    }
}

int16_t PluckSynth_NextSample(void) {
    float trig = 0.0f;

    // Применяем накопленные изменения параметров в аудиопотоке по счетчику поколений
    uint32_t current_seq = params_sequence;
    if (current_seq != applied_sequence) {
        string_voice.SetFreq(pending_freq);
        string_voice.SetAmp(pending_amp);
        string_voice.SetDecay(active_decay);
        string_voice.SetDamp(pending_damp);
        applied_sequence = current_seq;
    }

    if (trigger_pending) {
        trig = 1.0f;
        trigger_pending = false;
    }

    // Вычисляем отсчёт физического моделирования струны (-1.0f .. +1.0f)
    float sample_f = string_voice.Process(trig) * PLUCK_MASTER_GAIN * 32767.0f;

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
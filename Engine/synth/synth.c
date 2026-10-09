#include "synth.h"
#include <stdint.h>
#include <math.h>
#include <stdbool.h>

#define SYNTH_SAMPLE_RATE 48000
#define SYNTH_MAX_FREQUENCY 20000.0f
#define WAVETABLE_SIZE    1024
#define WAVETABLE_MASK    (WAVETABLE_SIZE - 1)

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static float wavetable[WAVETABLE_SIZE];
static uint32_t phase_accumulator = 0;

// volatile необходим, так как параметры меняются из main(), а читаются в прерывании DMA
static volatile uint32_t phase_increment = 0;
static volatile float current_amplitude = 0.25f;
static volatile bool is_playing = false;

void Synth_Init(void) {
    for (int i = 0; i < WAVETABLE_SIZE; i++) {
        wavetable[i] = sinf(2.0f * (float)M_PI * (float)i / (float)WAVETABLE_SIZE);
    }
	phase_accumulator = 0;
    Synth_SetFrequency(440.0f);
    Synth_SetAmplitude(0.25f);
    Synth_Stop();
}

void Synth_SetFrequency(float frequency) {
    if (!isfinite(frequency) || frequency < 0.0f) {
        frequency = 0.0f;
    } else if (frequency > SYNTH_MAX_FREQUENCY) {
        frequency = SYNTH_MAX_FREQUENCY;
    }

    float inc = (frequency * 4294967296.0f)
                / (float)SYNTH_SAMPLE_RATE;

    phase_increment = (uint32_t)inc;
}

void Synth_SetAmplitude(float amplitude) {
    if (amplitude < 0.0f) {
        current_amplitude = 0.0f; // Амплитуда обычно неотрицательна [0.0 .. 1.0]
    } else if (amplitude > 1.0f) {
        current_amplitude = 1.0f;
    } else {
        current_amplitude = amplitude;
    }
}

void Synth_Start(void) {
    is_playing = true;
}

void Synth_Stop(void) {
    is_playing = false;
    // Не трогаем phase_accumulator здесь, чтобы избежать гонки данных с прерыванием DMA
}

int16_t Synth_NextSample(void) {
    if (!is_playing) {
        return 0;
    }

    // 1. Целая часть индекса (старшие 10 бит)
    uint32_t index = phase_accumulator >> 22;
    uint32_t next_index = (index + 1) & WAVETABLE_MASK;

    // 2. Дробная часть фазы для линейной интерполяции (следующие 16 бит: биты 6..21)
    float frac = (float)((phase_accumulator >> 6) & 0xFFFF) * (1.0f / 65536.0f);

    // 3. Линейная интерполяция между двумя соседними сэмплами таблицы
    float s0 = wavetable[index];
    float s1 = wavetable[next_index];
    float sample_f = s0 + frac * (s1 - s0);

    // Продвигаем фазу
    phase_accumulator += phase_increment;

    // Применяем амплитуду и масштабируем к int16_t
    sample_f *= current_amplitude * 32767.0f;

    if (sample_f > 32767.0f) {
        sample_f = 32767.0f;
    } else if (sample_f < -32768.0f) {
        sample_f = -32768.0f;
    }

    return (int16_t)sample_f;
}
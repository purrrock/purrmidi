#include "synth.h"
#include <math.h>
#include <stdbool.h>

#define SYNTH_SAMPLE_RATE 48000
#define WAVETABLE_SIZE 1024

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static float wavetable[WAVETABLE_SIZE];
static uint32_t phase_accumulator = 0;
static uint32_t phase_increment = 0;
static float current_amplitude = 0.25f;
static bool is_playing = false;

void Synth_Init(void) {
    // Populate the sine wavetable
    for (int i = 0; i < WAVETABLE_SIZE; i++) {
        wavetable[i] = sinf(2.0f * (float)M_PI * (float)i / (float)WAVETABLE_SIZE);
    }

    // Set initial values
    Synth_SetFrequency(440.0f);
    Synth_SetAmplitude(0.25f);
    Synth_Stop();
}

void Synth_SetFrequency(float frequency) {
    if (frequency < 0.0f) {
        frequency = 0.0f;
    }

    // Calculate phase increment
    // phase_increment = (frequency * 2^32) / SAMPLE_RATE
    float inc = (frequency * 4294967296.0f) / (float)SYNTH_SAMPLE_RATE;
    phase_increment = (uint32_t)inc;
}

void Synth_SetAmplitude(float amplitude) {
    if (amplitude < -1.0f) {
        current_amplitude = -1.0f;
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
    phase_accumulator = 0;
}

int16_t Synth_NextSample(void) {
    if (!is_playing) {
        return 0;
    }

    // Get the index into the wavetable from the top bits of the accumulator
    // Since wavetable size is 1024 (2^10), we shift down by 32 - 10 = 22 bits
    uint32_t index = phase_accumulator >> (32 - 10);

    // Fetch the sample from wavetable
    float sample_f = wavetable[index];

    // Advance the phase accumulator
    phase_accumulator += phase_increment;

    // Apply amplitude
    sample_f *= current_amplitude;

    // Scale to int16_t range
    sample_f *= 32767.0f;

    // Clamp to int16_t range to prevent overflow
    if (sample_f > 32767.0f) {
        sample_f = 32767.0f;
    } else if (sample_f < -32768.0f) {
        sample_f = -32768.0f;
    }

    return (int16_t)sample_f;
}

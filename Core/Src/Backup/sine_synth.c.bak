#include "sine_synth.h"
#include "synth.h"
#include <stdbool.h>

#define SINE_SAMPLE_RATE      48000.0f
#define SINE_ATTACK_SEC       0.005f      /* линейная атака  */
#define SINE_RELEASE_SEC      0.050f      /* линейный релиз  */
#define SINE_ATTACK_STEP      (1.0f / (SINE_SAMPLE_RATE * SINE_ATTACK_SEC))
#define SINE_RELEASE_STEP     (1.0f / (SINE_SAMPLE_RATE * SINE_RELEASE_SEC))
#define SINE_MAX_AMPLITUDE    0.5f        /* запас по громкости */

/* Параметры пишутся из main, читаются в прерывании DMA: все — 32-битные volatile. */
static volatile int16_t  g_note        = -1;
static volatile bool     g_gate        = false;
static volatile bool     g_sustain     = false;
static volatile float    g_volume      = 1.0f;

/* Состояние только аудиоконтекста */
static float g_env = 0.0f;

static inline float midi_to_freq(uint8_t note)
{
    /* 440 * 2^((n - 69) / 12) без powf: таблица полутонов в пределах октавы + сдвиг по степеням 2 */
    static const float semitone[12] = {
        1.0f, 1.059463094f, 1.122462048f, 1.189207115f, 1.259921050f, 1.334839854f,
        1.414213562f, 1.498307077f, 1.587401052f, 1.681792831f, 1.781797436f, 1.887748625f
    };
    int n = (int)note - 60;          /* C4 = 261.6256 Hz */
    int oct = 0;
    while (n < 0)  { n += 12; oct--; }
    while (n >= 12) { n -= 12; oct++; }
    float f = 261.6255653f * semitone[n];
    while (oct > 0) { f *= 2.0f; oct--; }
    while (oct < 0) { f *= 0.5f; oct++; }
    return f;
}

void SineSynth_Init(void)
{
    Synth_Init();
    g_note    = -1;
    g_gate    = false;
    g_sustain = false;
    g_volume  = 1.0f;
    g_env     = 0.0f;
}

void SineSynth_NoteOn(uint8_t midi_note, uint8_t velocity)
{
    if (velocity == 0) {
        SineSynth_NoteOff(midi_note);
        return;
    }
    if (midi_note > 127) {
        return;
    }

    float amp = SINE_MAX_AMPLITUDE * (0.2f + 0.8f * ((float)velocity * (1.0f / 127.0f)));

    Synth_SetFrequency(midi_to_freq(midi_note));
    Synth_SetAmplitude(amp);
    Synth_Start();

    g_note = (int16_t)midi_note;
    g_gate = true;
}

void SineSynth_NoteOff(uint8_t midi_note)
{
    if (g_note == (int16_t)midi_note) {
        g_gate = false;
    }
}

void SineSynth_ControlChange(uint8_t control, uint8_t value)
{
    switch (control) {
        case 7:   /* Channel volume */
            g_volume = (float)value * (1.0f / 127.0f);
            break;
        case 64:  /* Sustain pedal */
            g_sustain = (value >= 64);
            break;
        case 120: /* All sound off */
        case 123: /* All notes off */
            g_gate = false;
            g_sustain = false;
            break;
        default:
            break;
    }
}

void SineSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames)
{
    for (uint32_t i = 0; i < num_frames; i++) {
        bool on = g_gate || g_sustain;

        if (on) {
            g_env += SINE_ATTACK_STEP;
            if (g_env > 1.0f) g_env = 1.0f;
        } else if (g_env > 0.0f) {
            g_env -= SINE_RELEASE_STEP;
            if (g_env < 0.0f) g_env = 0.0f;
        }

        int16_t s = 0;
        if (g_env > 0.0f) {
            s = (int16_t)((float)Synth_NextSample() * g_env * g_volume);
        }

        buffer[i * 2]     = s;
        buffer[i * 2 + 1] = s;
    }
}

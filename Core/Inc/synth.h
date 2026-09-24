#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>

/*
 * Synth Module
 *
 * Audio format: 48 kHz / signed 16-bit / mono
 */

void Synth_Init(void);
int16_t Synth_NextSample(void);
void Synth_SetFrequency(float frequency);
void Synth_SetAmplitude(float amplitude);
void Synth_Start(void);
void Synth_Stop(void);

#endif /* SYNTH_H */

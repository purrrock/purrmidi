#ifndef SYNTH_H
#define SYNTH_H

#include <stdint.h>

/**
 * @file    synth.h
 * @brief   Базовый генератор синусоидального аудиосигнала на основе табличного синтеза.
 *
 * Формат аудио: 48 кГц / signed 16-бит / моно.
 */

/** Инициализация таблиц волновых форм и начального состояния генератора. */
void Synth_Init(void);

/** Вычисление следующего отсчёта аудиосигнала (16-битный PCM). */
int16_t Synth_NextSample(void);

/** Установка частоты генерируемого сигнала в Гц. */
void Synth_SetFrequency(float frequency);

/** Установка амплитуды сигнала [0.0 .. 1.0]. */
void Synth_SetAmplitude(float amplitude);

/** Запуск воспроизведения генератора. */
void Synth_Start(void);

/** Остановка воспроизведения генератора. */
void Synth_Stop(void);

#endif /* SYNTH_H */

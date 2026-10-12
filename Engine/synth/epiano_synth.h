#ifndef EPIANO_SYNTH_H
#define EPIANO_SYNTH_H

#include <stdint.h>

/**
 * @file    epiano_synth.h
 * @brief   Полифоническое FM-электропиано (в духе Rhodes / DX7 "E.PIANO 1").
 *
 * Архитектура голоса (две параллельные FM-пары "модулятор -> несущая", все операторы — синусы):
 *
 *     BODY  (корпус звука): модулятор 1:1 -> несущая 1:1.
 *           Индекс модуляции зависит от velocity и затухает ("bark" —
 *           хриплый, яркий звук при сильном ударе, который смягчается).
 *     TINE  (язычок/молоточек): модулятор 14:1 -> несущая 1:1.
 *           Короткий высокочастотный "звон" атаки, быстро затухает.
 *
 *     voice = amp_env * velocity * (BODY + tine_level * TINE)
 *
 * Прочее: 8 голосов (EPIANO_NUM_VOICES), динамика по velocity, затухание
 * зависит от высоты ноты (басы звучат дольше), демпфер при отпускании клавиши,
 * педаль сустейна, лёгкая стереопанорама по положению клавиши, мягкий лимитер.
 *
 * Все вычисления — float, без использования динамической памяти (malloc) и без блокировок.
 * Входящие MIDI-события из основного цикла передаются в аудиопоток через lock-free SPSC FIFO
 * и применяются к состояниям голосов в самом начале каждого вызова EPianoSynth_FillStereoBuffer().
 *
 * Поддерживаемые MIDI CC:
 *     CC1, CC71, CC74  яркость (индекс FM-модуляции), 64 = штатная
 *     CC7              громкость
 *     CC64             педаль сустейна
 *     CC72             время релиза (демпфера)
 *     CC120, CC123     All Sound Off / All Notes Off
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Инициализация движка FM-электропиано и сброс его состояния. */
void EPianoSynth_Init(void);

/** Обработка события Note On (velocity 0 трактуется как NoteOff). */
void EPianoSynth_NoteOn(uint8_t midi_note, uint8_t velocity);

/** Обработка события Note Off. */
void EPianoSynth_NoteOff(uint8_t midi_note);

/** Обработка событий MIDI Control Change. */
void EPianoSynth_ControlChange(uint8_t control, uint8_t value);

/**
 * Блочное заполнение стереобуфера (L, R, L, R...), 48 кГц, signed 16-bit PCM.
 * Вызывается из прерывания DMA (STM32) или аудио-потока (ПК).
 */
void EPianoSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/** Количество MIDI-событий, потерянных из-за переполнения FIFO (для отладки). */
uint32_t EPianoSynth_GetDroppedEventCount(void);

/** Количество голосов, звучащих в данный момент (для отладки/индикации). */
uint32_t EPianoSynth_GetActiveVoiceCount(void);

#ifdef __cplusplus
}
#endif

#endif /* EPIANO_SYNTH_H */

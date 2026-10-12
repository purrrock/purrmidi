#ifndef EPIANO_SYNTH_H
#define EPIANO_SYNTH_H

#include <stdint.h>

/**
 * @file    epiano_synth.h
 * @brief   Полифоническое FM-электропиано (похожее на Fender Rhodes / DX7 "E.PIANO 1").
 *
 * Архитектура голоса (две параллельные FM-пары "модулятор -> несущая"):
 *
 *     BODY (основное тело звука): модулятор 1:1 -> несущая 1:1.
 *          Индекс FM-модуляции зависит от динамики нажатия (velocity) и плавно
 *          затухает ("bark" — характерный звонкий/хриплый звук атаки).
 *
 *     TINE (атака язычка/молоточка): модулятор 14:1 -> несущая 1:1.
 *          Короткий высокочастотный перкуссионный щелчок атаки.
 *
 *     voice = amp_env * velocity * (BODY + tine_level * TINE)
 *
 * Особенности:
 *     - Полифония: 8 голосов (EPIANO_NUM_VOICES) с алгоритмом перехвата (voice stealing);
 *     - Затухание зависит от высоты ноты (басовые струны/язычки звучат дольше);
 *     - Поддержка педали сустейна (CC 64) и регулировки времени демпфера;
 *     - Стереопанорамирование по позиции ноты на клавиатуре;
 *     - Мягкий лимитер выходного сигнала для защиты от жесткого клиппинга;
 *     - Передача событий через lock-free SPSC FIFO из главного цикла в контекст аудио.
 *
 * Поддерживаемые MIDI Control Change:
 *     CC 1, CC 71, CC 74 : Яркость (коэффициент индекса FM-модуляции, по умолчанию 64)
 *     CC 7              : Громкость канала
 *     CC 64             : Педаль сустейна
 *     CC 72             : Время затухания релиза (демпфера)
 *     CC 120, CC 123    : All Sound Off / All Notes Off
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Инициализация движка FM-электропиано и сброс его состояния. */
void EPianoSynth_Init(void);

/** Обработка события Note On (velocity 0 эквивалентен Note Off). */
void EPianoSynth_NoteOn(uint8_t midi_note, uint8_t velocity);

/** Обработка события Note Off. */
void EPianoSynth_NoteOff(uint8_t midi_note);

/** Обработка событий MIDI Control Change. */
void EPianoSynth_ControlChange(uint8_t control, uint8_t value);

/**
 * Блочное заполнение стереобуфера (L, R, L, R...), 48 кГц, signed 16-bit PCM.
 * Вызывается из аудиопрерывания (DMA) или аудиопотока.
 */
void EPianoSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/** Возвращает количество событий, потерянных из-за переполнения FIFO. */
uint32_t EPianoSynth_GetDroppedEventCount(void);

/** Возвращает количество активных голосов в данный момент. */
uint32_t EPianoSynth_GetActiveVoiceCount(void);

#ifdef __cplusplus
}
#endif

#endif /* EPIANO_SYNTH_H */

#ifndef SYNTH_ENGINE_H
#define SYNTH_ENGINE_H

#include <stdint.h>

/*
 * Единый интерфейс звукового движка PurrMidi.
 *
 * В прошивку (и в Windows-версию) всегда входят ВСЕ синтезаторы, а активный
 * выбирается во время работы сообщением MIDI Program Change (0xC0):
 *
 *     Program | Синтезатор
 *     --------+--------------------------------------------------------
 *        0    | E-Piano  — полифоническое FM-электропиано (по умолчанию)
 *        1    | Pluck    — Карплус-Стронг, DaisySP Pluck
 *        2    | Sine     — одноголосая синусоида (проверка тракта)
 *
 * Номера программ вне диапазона заворачиваются по модулю SYNTH_ENGINE_COUNT
 * (program % 3), поэтому кнопки «+/−» на любой клавиатуре всегда выбирают
 * какой-нибудь инструмент, а не «молчат». Канал сообщения не учитывается (omni).
 *
 * Состояние выбора в энергонезависимой памяти НЕ сохраняется: после включения
 * питания всегда звучит E-Piano.
 *
 * Формат аудио: 48 кГц, signed 16 бит, стерео с чередованием (L, R, L, R...).
 *
 * Контекст вызовов (контракт одинаков для всех движков):
 *   SynthEngine_Init / NoteOn / NoteOff / ControlChange / ProgramChange / Select
 *       — из основного цикла (main / MIDI-поток), только из ОДНОГО контекста;
 *   SynthEngine_FillStereoBuffer
 *       — из аудио-прерывания DMA (STM32) или аудио-потока (ПК).
 *
 * Переключение движка не блокирует аудио-прерывание: пока идёт смена,
 * FillStereoBuffer просто отдаёт тишину (см. synth_engine.cpp).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Порядок элементов = номер MIDI-программы (Program Change). */
typedef enum {
    SYNTH_ENGINE_EPIANO = 0,
    SYNTH_ENGINE_PLUCK  = 1,
    SYNTH_ENGINE_SINE   = 2,
    SYNTH_ENGINE_COUNT
} SynthEngineId;

/* Синтезатор, который звучит после включения питания. */
#define SYNTH_ENGINE_DEFAULT SYNTH_ENGINE_EPIANO

/** Сброс и включение синтезатора по умолчанию (E-Piano). Можно вызывать повторно. */
void SynthEngine_Init(void);

void SynthEngine_NoteOn(uint8_t midi_note, uint8_t velocity);   /* velocity 0 == NoteOff */
void SynthEngine_NoteOff(uint8_t midi_note);
void SynthEngine_ControlChange(uint8_t control, uint8_t value);

/**
 * MIDI Program Change: выбор синтезатора по номеру программы (program % SYNTH_ENGINE_COUNT).
 * Если нужный синтезатор уже активен — ничего не происходит (звучащие ноты не обрываются).
 */
void SynthEngine_ProgramChange(uint8_t program);

/**
 * Явный выбор синтезатора. Новый движок стартует «с нуля» (все ноты сброшены).
 * @return 0, если id вне диапазона; иначе 1.
 */
int SynthEngine_Select(SynthEngineId id);

/** Текущий активный синтезатор. */
SynthEngineId SynthEngine_GetCurrent(void);

/** Имя синтезатора: "epiano" / "pluck" / "sine" (для логов). Для неверного id — "?". */
const char *SynthEngine_GetEngineName(SynthEngineId id);

/** Имя активного синтезатора. */
const char *SynthEngine_GetName(void);

/** Блочное заполнение стереобуфера (L, R, L, R...). */
void SynthEngine_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_ENGINE_H */

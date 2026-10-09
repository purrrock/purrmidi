#ifndef SYNTH_ENGINE_H
#define SYNTH_ENGINE_H

/*
 * Единый интерфейс звукового движка PurrMidi.
 *
 * Конкретный синтезатор выбирается на ЭТАПЕ КОМПИЛЯЦИИ (прошивка STM32 и
 * Windows-версия используют один и тот же механизм):
 *
 *     cmake ... -DPURRMIDI_SYNTH=sine     (одноголосая синусоида)
 *     cmake ... -DPURRMIDI_SYNTH=pluck    (Карплус-Стронг, DaisySP Pluck) — по умолчанию
 *     cmake ... -DPURRMIDI_SYNTH=epiano   (полифоническое FM-электропиано)
 *
 * CMake определяет ровно один из макросов PURRMIDI_SYNTH_SINE / _PLUCK / _EPIANO.
 * Если не определён ни один (например, файл собирается вне CMake), используется
 * pluck — как и было до появления выбора синтезатора.
 *
 * Все функции — простые макро-алиасы на функции выбранного движка, т.е.
 * накладных расходов нет, а остальной код (MIDI-диспетчер, main.c, Windows-приложение)
 * не зависит от конкретной реализации.
 *
 * Формат аудио: 48 кГц, signed 16 бит, стерео с чередованием (L, R, L, R...).
 */

#if !defined(PURRMIDI_SYNTH_SINE) && !defined(PURRMIDI_SYNTH_PLUCK) && !defined(PURRMIDI_SYNTH_EPIANO)
#define PURRMIDI_SYNTH_PLUCK 1
#endif

#if (defined(PURRMIDI_SYNTH_SINE) + defined(PURRMIDI_SYNTH_PLUCK) + defined(PURRMIDI_SYNTH_EPIANO)) != 1
#error "Нужно определить ровно один из PURRMIDI_SYNTH_SINE / PURRMIDI_SYNTH_PLUCK / PURRMIDI_SYNTH_EPIANO"
#endif

#if defined(PURRMIDI_SYNTH_SINE)
    #include "sine_synth.h"
    #define SYNTH_ENGINE_NAME                   "sine"
    #define SynthEngine_Init                    SineSynth_Init
    #define SynthEngine_NoteOn                  SineSynth_NoteOn
    #define SynthEngine_NoteOff                 SineSynth_NoteOff
    #define SynthEngine_ControlChange           SineSynth_ControlChange
    #define SynthEngine_FillStereoBuffer        SineSynth_FillStereoBuffer
#elif defined(PURRMIDI_SYNTH_PLUCK)
    #include "pluck_synth.h"
    #define SYNTH_ENGINE_NAME                   "pluck"
    #define SynthEngine_Init                    PluckSynth_Init
    #define SynthEngine_NoteOn                  PluckSynth_NoteOn
    #define SynthEngine_NoteOff                 PluckSynth_NoteOff
    #define SynthEngine_ControlChange           PluckSynth_ControlChange
    #define SynthEngine_FillStereoBuffer        PluckSynth_FillStereoBuffer
#else /* PURRMIDI_SYNTH_EPIANO */
    #include "epiano_synth.h"
    #define SYNTH_ENGINE_NAME                   "epiano"
    #define SynthEngine_Init                    EPianoSynth_Init
    #define SynthEngine_NoteOn                  EPianoSynth_NoteOn
    #define SynthEngine_NoteOff                 EPianoSynth_NoteOff
    #define SynthEngine_ControlChange           EPianoSynth_ControlChange
    #define SynthEngine_FillStereoBuffer        EPianoSynth_FillStereoBuffer
#endif

/*
 * Контракт, одинаковый для всех движков:
 *
 *   void SynthEngine_Init(void);
 *   void SynthEngine_NoteOn(uint8_t midi_note, uint8_t velocity);   // velocity 0 == NoteOff
 *   void SynthEngine_NoteOff(uint8_t midi_note);
 *   void SynthEngine_ControlChange(uint8_t control, uint8_t value);
 *   void SynthEngine_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);
 *
 * NoteOn/NoteOff/ControlChange вызываются из основного цикла (контекст main / MIDI-потока),
 * FillStereoBuffer — из аудио-прерывания DMA (или аудио-потока на ПК).
 * Передача событий между этими контекстами реализована внутри движка без блокировок.
 */

#endif /* SYNTH_ENGINE_H */

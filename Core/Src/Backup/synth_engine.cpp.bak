/*
 * Рантайм-диспетчер звуковых движков: sine / pluck / epiano.
 * Выбор движка делается MIDI-сообщением Program Change (см. synth_engine.h).
 *
 * Потоковая модель
 * ----------------
 * Состояние каждого движка принадлежит аудиоконтексту, а события в него
 * приходят из main через собственные lock-free FIFO. Смена движка вызывает
 * Init() нового движка (сброс его FIFO и голосов), поэтому она не должна идти
 * одновременно с FillStereoBuffer(). Для этого используется флаг-замок g_busy:
 *
 *   - FillStereoBuffer берёт замок через test_and_set БЕЗ ожидания; если замок
 *     занят (идёт смена движка) — отдаёт тишину и сразу возвращается. Поэтому
 *     аудио-прерывание на STM32 никогда не зависает на замке, который держит main.
 *   - Select() берёт замок с ожиданием. На STM32 прерывание выполняется до конца
 *     и не может быть прервано main, так что замок гарантированно свободен;
 *     на ПК аудио-поток держит замок лишь на время одного блока.
 */

#include "synth_engine.h"
#include "epiano_synth.h"
#include "organ_synth.h"
#include "pluck_synth.h"
#include "sine_synth.h"

#include <atomic>
#include <cstring>

namespace {

struct Engine {
    const char *name;
    void (*init)(void);
    void (*note_on)(uint8_t, uint8_t);
    void (*note_off)(uint8_t);
    void (*control_change)(uint8_t, uint8_t);
    void (*fill)(int16_t *, uint32_t);
};

/* Индекс в таблице == SynthEngineId == номер MIDI-программы. */
const Engine kEngines[SYNTH_ENGINE_COUNT] = {
    /* SYNTH_ENGINE_EPIANO */
    { "epiano", EPianoSynth_Init, EPianoSynth_NoteOn, EPianoSynth_NoteOff,
      EPianoSynth_ControlChange, EPianoSynth_FillStereoBuffer },
    /* SYNTH_ENGINE_PLUCK */
    { "pluck", PluckSynth_Init, PluckSynth_NoteOn, PluckSynth_NoteOff,
      PluckSynth_ControlChange, PluckSynth_FillStereoBuffer },
    /* SYNTH_ENGINE_SINE */
    { "sine", SineSynth_Init, SineSynth_NoteOn, SineSynth_NoteOff,
      SineSynth_ControlChange, SineSynth_FillStereoBuffer },
    /* SYNTH_ENGINE_ORGAN */
    { "organ", OrganSynth_Init, OrganSynth_NoteOn, OrganSynth_NoteOff,
      OrganSynth_ControlChange, OrganSynth_FillStereoBuffer },
};

std::atomic<const Engine *> g_active{nullptr};   /* nullptr до первого SynthEngine_Init() */
std::atomic_flag            g_busy = ATOMIC_FLAG_INIT;

/* Включает движок: сбрасывает его состояние и делает активным. Только из main-контекста. */
void activate(SynthEngineId id)
{
    const Engine *next = &kEngines[id];

    while (g_busy.test_and_set(std::memory_order_acquire)) {
        /* ждём, пока аудио-поток (ПК) закончит текущий блок */
    }
    next->init();
    g_active.store(next, std::memory_order_relaxed);
    g_busy.clear(std::memory_order_release);
}

} // namespace

extern "C" {

void SynthEngine_Init(void)
{
    activate(SYNTH_ENGINE_DEFAULT);
}

int SynthEngine_Select(SynthEngineId id)
{
    if ((unsigned)id >= (unsigned)SYNTH_ENGINE_COUNT) {
        return 0;
    }
    if (g_active.load(std::memory_order_relaxed) != &kEngines[id]) {
        activate(id);
    }
    return 1;
}

void SynthEngine_ProgramChange(uint8_t program)
{
    SynthEngine_Select((SynthEngineId)(program % SYNTH_ENGINE_COUNT));
}

SynthEngineId SynthEngine_GetCurrent(void)
{
    const Engine *e = g_active.load(std::memory_order_relaxed);
    return e ? (SynthEngineId)(e - kEngines) : SYNTH_ENGINE_DEFAULT;
}

const char *SynthEngine_GetEngineName(SynthEngineId id)
{
    return ((unsigned)id < (unsigned)SYNTH_ENGINE_COUNT) ? kEngines[id].name : "?";
}

const char *SynthEngine_GetName(void)
{
    return SynthEngine_GetEngineName(SynthEngine_GetCurrent());
}

void SynthEngine_NoteOn(uint8_t midi_note, uint8_t velocity)
{
    const Engine *e = g_active.load(std::memory_order_relaxed);
    if (e) {
        e->note_on(midi_note, velocity);
    }
}

void SynthEngine_NoteOff(uint8_t midi_note)
{
    const Engine *e = g_active.load(std::memory_order_relaxed);
    if (e) {
        e->note_off(midi_note);
    }
}

void SynthEngine_ControlChange(uint8_t control, uint8_t value)
{
    const Engine *e = g_active.load(std::memory_order_relaxed);
    if (e) {
        e->control_change(control, value);
    }
}

void SynthEngine_FillStereoBuffer(int16_t *buffer, uint32_t num_frames)
{
    if (g_busy.test_and_set(std::memory_order_acquire)) {
        /* Идёт смена синтезатора: отдаём тишину, не ожидая замка. */
        std::memset(buffer, 0, (size_t)num_frames * 2U * sizeof(int16_t));
        return;
    }

    const Engine *e = g_active.load(std::memory_order_relaxed);
    if (e) {
        e->fill(buffer, num_frames);
    } else {
        std::memset(buffer, 0, (size_t)num_frames * 2U * sizeof(int16_t));
    }

    g_busy.clear(std::memory_order_release);
}

} // extern "C"

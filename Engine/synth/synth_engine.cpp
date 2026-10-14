/*
 * Рантайм-диспетчер звуковых движков: epiano / pluck / lead / organ / soundfont.
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
#include "lead_synth.h"
#include "organ_synth.h"
#include "pluck_synth.h"
#include "soundfont_synth.h"
#include "dulcimer_synth.h"

#include <atomic>
#include <cstring>
#include <cstdint>

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
    /* SYNTH_ENGINE_LEAD */
    { "lead", LeadSynth_Init, LeadSynth_NoteOn, LeadSynth_NoteOff,
      LeadSynth_ControlChange, LeadSynth_FillStereoBuffer },
    /* SYNTH_ENGINE_ORGAN */
    { "organ", OrganSynth_Init, OrganSynth_NoteOn, OrganSynth_NoteOff,
      OrganSynth_ControlChange, OrganSynth_FillStereoBuffer },
    /* SYNTH_ENGINE_SOUNDFONT */
    { "soundfont", SoundFontSynth_Init, SoundFontSynth_NoteOn, SoundFontSynth_NoteOff,
      SoundFontSynth_ControlChange, SoundFontSynth_FillStereoBuffer },
    /* SYNTH_ENGINE_DULCIMER */
    { "dulcimer", DulcimerSynth_Init, DulcimerSynth_NoteOn, DulcimerSynth_NoteOff,
      DulcimerSynth_ControlChange, DulcimerSynth_FillStereoBuffer },
};

std::atomic<const Engine *> g_active{nullptr};   /* nullptr до первого SynthEngine_Init() */
std::atomic_flag            g_busy = ATOMIC_FLAG_INIT;

/*
 * Общая (master) громкость. PCM5102A не имеет регистров громкости (у него нет ни I2C, ни SPI),
 * поэтому громкость регулируется программно: после рендера активного движка выходные
 * отсчёты умножаются на коэффициент. Значение — MIDI-уровень 0..127 (CC SYNTH_MASTER_VOLUME_CC).
 * Пишется из main, читается из аудио-контекста; атомарно, без замков.
 */
std::atomic<uint8_t> g_master_vol{SYNTH_MASTER_VOLUME_DEFAULT};
float                g_master_gain_applied = 1.0f;   /* коэффициент в конце прошлого блока (только аудио-контекст) */

/* Уровень 0..127 -> линейный коэффициент. Квадратичная кривая ≈ естественная для слуха; 127 -> 1.0, 0 -> 0. */
float volume_to_gain(uint8_t v)
{
    const float n = (float)v * (1.0f / 127.0f);
    return n * n;
}

/* Применяет master-громкость к блоку. Коэффициент линейно меняется от прошлого значения к новому
 * внутри блока, поэтому при вращении ручки нет «ступенек» и щелчков. */
void apply_master_volume(int16_t *buffer, uint32_t num_frames)
{
    const float target = volume_to_gain(g_master_vol.load(std::memory_order_relaxed));
    float       g      = g_master_gain_applied;

    if (g == target && target == 1.0f) {
        return;   /* единичное усиление: ничего не делаем */
    }

    const float step = (num_frames > 0U) ? (target - g) / (float)num_frames : 0.0f;
    for (uint32_t i = 0; i < num_frames; i++) {
        g += step;
        const int32_t l = (int32_t)((float)buffer[2U * i]      * g);
        const int32_t r = (int32_t)((float)buffer[2U * i + 1U] * g);
        buffer[2U * i]      = (int16_t)(l > 32767 ? 32767 : (l < -32768 ? -32768 : l));
        buffer[2U * i + 1U] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }
    g_master_gain_applied = target;
}

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
    g_master_vol.store(SYNTH_MASTER_VOLUME_DEFAULT, std::memory_order_relaxed);
    g_master_gain_applied = volume_to_gain(SYNTH_MASTER_VOLUME_DEFAULT);
    activate(SYNTH_ENGINE_DEFAULT);
}

void SynthEngine_SetMasterVolume(uint8_t level)
{
    g_master_vol.store(level > 127U ? (uint8_t)127U : level, std::memory_order_relaxed);
}

uint8_t SynthEngine_GetMasterVolume(void)
{
    return g_master_vol.load(std::memory_order_relaxed);
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

SynthEngineId SynthEngine_SelectNext(void)
{
    SynthEngineId cur = SynthEngine_GetCurrent();
    SynthEngineId next = cur;
    for (uint32_t i = 0; i < (uint32_t)SYNTH_ENGINE_COUNT; i++) {
        next = (SynthEngineId)(((uint32_t)next + 1U) % SYNTH_ENGINE_COUNT);
        if (SynthEngine_IsAvailable(next)) {
            break;
        }
    }
    SynthEngine_Select(next);
    return next;
}

void SynthEngine_ProgramChange(uint8_t program)
{
    SynthEngine_Select((SynthEngineId)(program % SYNTH_ENGINE_COUNT));
}

int SynthEngine_IsAvailable(SynthEngineId id)
{
    if ((unsigned)id >= (unsigned)SYNTH_ENGINE_COUNT) {
        return 0;
    }
    if (id == SYNTH_ENGINE_SOUNDFONT) {
        return SoundFontSynth_IsAvailable() ? 1 : 0;
    }
    return 1;
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
    if (control == SYNTH_MASTER_VOLUME_CC) {
        /* Общая громкость обрабатывается здесь и в движки не передаётся (чтобы не применялась дважды). */
        SynthEngine_SetMasterVolume(value);
        return;
    }

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
        apply_master_volume(buffer, num_frames);
    } else {
        std::memset(buffer, 0, (size_t)num_frames * 2U * sizeof(int16_t));
    }

    g_busy.clear(std::memory_order_release);
}

} // extern "C"

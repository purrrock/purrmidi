/*
 * Рантайм-диспетчер звуковых движков: epiano / pluck / lead / organ / dulcimer / flute / saxophone / soundfont
 * (пресеты SF2 — отдельные программы в конце списка).
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
 *     аудио-контекст на STM32 (PendSV) никогда не зависает на замке, который держит main.
 *   - Select() берёт замок с ожиданием. На STM32 рендер идёт в PendSV, который
 *     выполняется до конца и не может быть прерван main (его вытесняют только
 *     другие прерывания, а они движки не трогают), так что замок гарантированно
 *     свободен; на ПК аудио-поток держит замок лишь на время одного блока.
 */

#include "synth_engine.h"
#include "synth_common.h"
#include "epiano_synth.h"
#include "lead_synth.h"
#include "organ_synth.h"
#include "pluck_synth.h"
#include "soundfont_synth.h"
#include "dulcimer_synth.h"
#include "flute_synth.h"
#include "saxophone_synth.h"

#include <atomic>
#include <cstdio>
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
    /* Необязательные обработчики (nullptr = движок не поддерживает, сообщение игнорируется). */
    void (*pitch_bend)(int16_t);
    void (*channel_pressure)(uint8_t);
    void (*poly_pressure)(uint8_t, uint8_t);
};

/* Индекс в таблице == SynthEngineId == номер MIDI-программы. */
const Engine kEngines[SYNTH_ENGINE_COUNT] = {
    /* SYNTH_ENGINE_EPIANO */
    { "epiano", EPianoSynth_Init, EPianoSynth_NoteOn, EPianoSynth_NoteOff,
      EPianoSynth_ControlChange, EPianoSynth_FillStereoBuffer, EPianoSynth_PitchBend, nullptr, nullptr },
    /* SYNTH_ENGINE_PLUCK */
    { "pluck", PluckSynth_Init, PluckSynth_NoteOn, PluckSynth_NoteOff,
      PluckSynth_ControlChange, PluckSynth_FillStereoBuffer, nullptr, nullptr, nullptr },
    /* SYNTH_ENGINE_LEAD */
    { "lead", LeadSynth_Init, LeadSynth_NoteOn, LeadSynth_NoteOff,
      LeadSynth_ControlChange, LeadSynth_FillStereoBuffer, nullptr, nullptr, nullptr },
    /* SYNTH_ENGINE_ORGAN */
    { "organ", OrganSynth_Init, OrganSynth_NoteOn, OrganSynth_NoteOff,
      OrganSynth_ControlChange, OrganSynth_FillStereoBuffer, nullptr, nullptr, nullptr },
    /* SYNTH_ENGINE_DULCIMER */
    { "dulcimer", DulcimerSynth_Init, DulcimerSynth_NoteOn, DulcimerSynth_NoteOff,
      DulcimerSynth_ControlChange, DulcimerSynth_FillStereoBuffer, nullptr, nullptr, nullptr },
    /* SYNTH_ENGINE_FLUTE */
    { "flute", FluteSynth_Init, FluteSynth_NoteOn, FluteSynth_NoteOff,
      FluteSynth_ControlChange, FluteSynth_FillStereoBuffer, nullptr, nullptr, nullptr },
    /* SYNTH_ENGINE_SAXOPHONE */
    { "saxophone", SaxophoneSynth_Init, SaxophoneSynth_NoteOn, SaxophoneSynth_NoteOff,
      SaxophoneSynth_ControlChange, SaxophoneSynth_FillStereoBuffer, nullptr, nullptr, nullptr },
    /* SYNTH_ENGINE_SOUNDFONT */
    { "soundfont", SoundFontSynth_Init, SoundFontSynth_NoteOn, SoundFontSynth_NoteOff,
      SoundFontSynth_ControlChange, SoundFontSynth_FillStereoBuffer, SoundFontSynth_PitchBend, nullptr, nullptr },
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

/* Индекс пресета SF2, выбранный пользователем (имеет смысл, пока активен SoundFont; хранится в main-контексте).
 * Переживает Panic() и повторную активацию движка. */
uint32_t             g_sf_preset = 0;

/* Короткие имена встроенных движков для дисплея (до 17 символов, ASCII). Порядок = SynthEngineId. */
const char * const kDisplayNames[SYNTH_ENGINE_SOUNDFONT] = {
    "E-Piano", "Pluck", "Lead", "Organ", "Dulcimer", "Flute", "Saxophone"
};

/* Число пресетов SF2, видимых в списке программ (минимум 1: «SoundFont» как место для ошибки при отсутствии банка). */
uint32_t sf_slot_count()
{
    const uint32_t n = SoundFontSynth_GetPresetCount();
    return n ? n : 1U;
}

/* Положение колеса высоты тона (-8192..8191). Запоминается здесь, чтобы смена движка (его Init() сбрасывает колесо)
 * не «отпускала» удерживаемое колесо. Пишется и читается только из main-контекста. */
int16_t              g_pitch_bend = 0;

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
    if (id == SYNTH_ENGINE_SOUNDFONT) {
        SoundFontSynth_SelectPreset(g_sf_preset);   /* Init() ставит пресет 0: возвращаем выбранный */
    }
    if (next->pitch_bend) {
        next->pitch_bend(g_pitch_bend);   /* колесо - физический орган: переносим его положение в новый движок */
    }
    g_active.store(next, std::memory_order_relaxed);
    g_busy.clear(std::memory_order_release);
}

} // namespace

extern "C" {

void SynthEngine_Init(void)
{
    g_master_vol.store(SYNTH_MASTER_VOLUME_DEFAULT, std::memory_order_relaxed);
    g_master_gain_applied = volume_to_gain(SYNTH_MASTER_VOLUME_DEFAULT);
    g_pitch_bend = 0;
    activate(SYNTH_ENGINE_DEFAULT);
}

void SynthEngine_Panic(void)
{
    g_pitch_bend = 0;   /* после отключения клавиатуры положение колеса неизвестно */
    const Engine *cur = g_active.load(std::memory_order_relaxed);
    if (cur) {
        activate((SynthEngineId)(cur - kEngines));
    }
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

uint32_t SynthEngine_GetProgramCount(void)
{
    return (uint32_t)SYNTH_ENGINE_SOUNDFONT + sf_slot_count();
}

uint32_t SynthEngine_GetProgram(void)
{
    const SynthEngineId id = SynthEngine_GetCurrent();
    return (id == SYNTH_ENGINE_SOUNDFONT) ? (uint32_t)SYNTH_ENGINE_SOUNDFONT + g_sf_preset : (uint32_t)id;
}

int SynthEngine_SelectProgram(uint32_t program)
{
    if (program >= SynthEngine_GetProgramCount()) {
        return 0;
    }
    if (program < (uint32_t)SYNTH_ENGINE_SOUNDFONT) {
        return SynthEngine_Select((SynthEngineId)program);
    }

    g_sf_preset = program - (uint32_t)SYNTH_ENGINE_SOUNDFONT;
    if (g_active.load(std::memory_order_relaxed) != &kEngines[SYNTH_ENGINE_SOUNDFONT]) {
        activate(SYNTH_ENGINE_SOUNDFONT);            /* внутри activate() применяется g_sf_preset */
    } else {
        SoundFontSynth_SelectPreset(g_sf_preset);    /* смена тембра без сброса звучащих нот */
    }
    return 1;
}

void SynthEngine_GetProgramName(uint32_t program, char *out, uint32_t out_size)
{
    if (!out || out_size == 0U) {
        return;
    }
    out[0] = '\0';

    if (program < (uint32_t)SYNTH_ENGINE_SOUNDFONT) {
        std::snprintf(out, out_size, "%s", kDisplayNames[program]);
        return;
    }

    const char *name = SoundFontSynth_GetPresetName(program - (uint32_t)SYNTH_ENGINE_SOUNDFONT);
    if (!name || !name[0]) {
        std::snprintf(out, out_size, "SoundFont");
        return;
    }
    std::snprintf(out, out_size, "SF:%s", name);

    /* Имя пресета в банке дополнено пробелами и может содержать не-ASCII: оставляем печатные символы ASCII. */
    for (uint32_t k = 0; out[k]; ++k) {
        if ((unsigned char)out[k] < 0x20U || (unsigned char)out[k] > 0x7EU) {
            out[k] = '?';
        }
    }
    uint32_t len = (uint32_t)std::strlen(out);
    while (len > 3U && out[len - 1U] == ' ') {
        out[--len] = '\0';
    }
}

SynthEngineId SynthEngine_SelectNext(void)
{
    const uint32_t total = SynthEngine_GetProgramCount();
    uint32_t       prog  = SynthEngine_GetProgram();

    for (uint32_t i = 0; i < total; i++) {
        prog = (prog + 1U) % total;
        const bool is_sf = prog >= (uint32_t)SYNTH_ENGINE_SOUNDFONT;
        if (!is_sf || SynthEngine_IsAvailable(SYNTH_ENGINE_SOUNDFONT)) {
            break;
        }
    }
    SynthEngine_SelectProgram(prog);
    return SynthEngine_GetCurrent();
}

void SynthEngine_ProgramChange(uint8_t program)
{
    SynthEngine_SelectProgram((uint32_t)program % SynthEngine_GetProgramCount());
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

void SynthEngine_PitchBend(int16_t bend)
{
    g_pitch_bend = bend < -8192 ? (int16_t)-8192 : (bend > 8191 ? (int16_t)8191 : bend);

    const Engine *e = g_active.load(std::memory_order_relaxed);
    if (e && e->pitch_bend) {
        e->pitch_bend(g_pitch_bend);
    }
}

void SynthEngine_ChannelPressure(uint8_t pressure)
{
    const Engine *e = g_active.load(std::memory_order_relaxed);
    if (e && e->channel_pressure) {
        e->channel_pressure(pressure > 127U ? (uint8_t)127U : pressure);
    }
}

void SynthEngine_PolyPressure(uint8_t midi_note, uint8_t pressure)
{
    const Engine *e = g_active.load(std::memory_order_relaxed);
    if (e && e->poly_pressure && midi_note < 128U) {
        e->poly_pressure(midi_note, pressure > 127U ? (uint8_t)127U : pressure);
    }
}

void SynthEngine_FillStereoBuffer(int16_t *buffer, uint32_t num_frames)
{
    if (g_busy.test_and_set(std::memory_order_acquire)) {
        /* Идёт смена синтезатора: отдаём тишину, не ожидая замка. */
        std::memset(buffer, 0, (size_t)num_frames * 2U * sizeof(int16_t));
        return;
    }

    synth::EnableFlushToZero();   /* денормали сильно замедляют затухающие хвосты (см. synth_common.h) */

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

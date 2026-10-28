/**
 * @file    purrmidi_web.cpp
 * @brief   Экспорт звуковых движков PurrMIDI в WebAssembly для веб-демо на сайте проекта.
 *
 * Собирается скриптом Platform/web/build.sh в site/demo/purrmidi.wasm. Код движков тот же, что в прошивке
 * и в Windows-приложении. Всё выполняется в одном потоке (AudioWorklet): MIDI-вызовы и pm_render() не
 * пересекаются, как того требует контракт synth_engine.h.
 *
 * Формат аудио: 48 кГц, int16, стерео с чередованием.
 */
#include "synth_engine.h"
#include "soundfont_synth.h"
#include "chord_detector.h"
#include "sf2_loader_web.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#define PM_EXPORT(name) __attribute__((export_name(name)))

namespace {

constexpr uint32_t kMaxFrames = 1024;

int16_t g_out[kMaxFrames * 2U];
char    g_text[64];
uint8_t g_held[128];
uint16_t g_held_count = 0;

} // namespace

extern "C" {

PM_EXPORT("pm_init") void pm_init(void)
{
    SynthEngine_Init();
    std::memset(g_held, 0, sizeof g_held);
    g_held_count = 0;
}

PM_EXPORT("pm_note_on") void pm_note_on(int note, int velocity)
{
    if (note < 0 || note > 127) return;
    if (velocity <= 0) {
        SynthEngine_NoteOff((uint8_t)note);
        if (g_held[note]) { g_held[note] = 0; g_held_count--; }
        return;
    }
    SynthEngine_NoteOn((uint8_t)note, (uint8_t)(velocity > 127 ? 127 : velocity));
    if (!g_held[note]) { g_held[note] = 1; g_held_count++; }
}

PM_EXPORT("pm_note_off") void pm_note_off(int note)
{
    if (note < 0 || note > 127) return;
    SynthEngine_NoteOff((uint8_t)note);
    if (g_held[note]) { g_held[note] = 0; g_held_count--; }
}

PM_EXPORT("pm_cc") void pm_cc(int control, int value)
{
    if (control < 0 || control > 127) return;
    SynthEngine_ControlChange((uint8_t)control, (uint8_t)(value < 0 ? 0 : (value > 127 ? 127 : value)));
}

PM_EXPORT("pm_pitch_bend") void pm_pitch_bend(int bend)
{
    SynthEngine_PitchBend((int16_t)bend);
}

PM_EXPORT("pm_channel_pressure") void pm_channel_pressure(int pressure)
{
    SynthEngine_ChannelPressure((uint8_t)(pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure)));
}

PM_EXPORT("pm_panic") void pm_panic(void)
{
    SynthEngine_Panic();
    std::memset(g_held, 0, sizeof g_held);
    g_held_count = 0;
}

/** Выбор программы по номеру (как Program Change без заворачивания). 1 - успех. */
PM_EXPORT("pm_select_program") int pm_select_program(int program)
{
    if (program < 0) return 0;
    const int ok = SynthEngine_SelectProgram((uint32_t)program);
    std::memset(g_held, 0, sizeof g_held);
    g_held_count = 0;
    return ok;
}

/** Настоящий MIDI Program Change из браузера (Web MIDI): 127 - следующий инструмент, остальное по модулю. */
PM_EXPORT("pm_program_change") void pm_program_change(int program)
{
    if (program < 0 || program > 127) return;
    if (program == 127) {
        (void)SynthEngine_SelectNext();
    } else {
        SynthEngine_ProgramChange((uint8_t)program);
    }
    std::memset(g_held, 0, sizeof g_held);
    g_held_count = 0;
}

PM_EXPORT("pm_program") int pm_program(void)
{
    return (int)SynthEngine_GetProgram();
}

PM_EXPORT("pm_program_count") int pm_program_count(void)
{
    return (int)SynthEngine_GetProgramCount();
}

/** Имя программы во внутреннем буфере (ASCII, завершается нулём). */
PM_EXPORT("pm_program_name") const char *pm_program_name(int program)
{
    SynthEngine_GetProgramName((uint32_t)program, g_text, sizeof g_text);
    return g_text;
}

PM_EXPORT("pm_volume") void pm_volume(int level)
{
    SynthEngine_SetMasterVolume((uint8_t)(level < 0 ? 0 : (level > 127 ? 127 : level)));
}

/** Рендер num_frames кадров (не больше 1024) во внутренний буфер. Возвращает указатель на int16 L,R,L,R... */
PM_EXPORT("pm_render") const int16_t *pm_render(int num_frames)
{
    const uint32_t n = (num_frames < 1) ? 1U : ((uint32_t)num_frames > kMaxFrames ? kMaxFrames : (uint32_t)num_frames);
    SynthEngine_FillStereoBuffer(g_out, n);
    return g_out;
}

/** Имя аккорда по удерживаемым нотам (как на дисплее платы); пустая строка, если аккорд не распознан. */
PM_EXPORT("pm_chord") const char *pm_chord(void)
{
    g_text[0] = '\0';
    if (g_held_count >= 3U) {
        if (!ChordDetector_Detect(g_held, g_held_count, g_text, sizeof g_text)) g_text[0] = '\0';
    }
    return g_text;
}

/* ---- SoundFont ---------------------------------------------------------------------------------------------- */

/**
 * Выделить в памяти модуля место под банк. JavaScript копирует туда файл, затем вызывает pm_sf2_commit().
 * Прежний банк освобождается, поэтому синтезатор SoundFont сначала сбрасывается (иначе он читал бы
 * освобождённую память); пока новый банк не принят, SoundFont молчит.
 */
PM_EXPORT("pm_sf2_alloc") uint8_t *pm_sf2_alloc(uint32_t size)
{
    SF2LoaderWeb_Unload();
    (void)SoundFontSynth_InitSF2();   /* банка нет: закрывает прежний разбор */
    return SF2LoaderWeb_Alloc(size);
}

/**
 * Разобрать банк. Возвращает число пресетов (0 - банк не подошёл).
 * Если SoundFont в этот момент звучит, его нужно выбрать заново (pm_select_program).
 */
PM_EXPORT("pm_sf2_commit") int pm_sf2_commit(void)
{
    const bool header_ok = SF2LoaderWeb_Commit();
    const bool parsed = SoundFontSynth_InitSF2();   /* всегда: сбрасывает прежний разбор банка */
    return (header_ok && parsed) ? (int)SoundFontSynth_GetPresetCount() : 0;
}

PM_EXPORT("pm_sf2_preset_count") int pm_sf2_preset_count(void)
{
    return (int)SoundFontSynth_GetPresetCount();
}

PM_EXPORT("pm_sf2_preset_name") const char *pm_sf2_preset_name(int index)
{
    const char *n = (index >= 0) ? SoundFontSynth_GetPresetName((uint32_t)index) : nullptr;
    std::snprintf(g_text, sizeof g_text, "%s", n ? n : "");
    return g_text;
}

} // extern "C"

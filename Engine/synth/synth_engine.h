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
 *        0    | E-Piano   — полифоническое FM-электропиано (по умолчанию)
 *        1    | Pluck     — Карплус-Стронг, DaisySP Pluck
 *        2    | Lead      — полифонический субтрактивный аналоговый лид
 *        3    | Organ     — орган Хаммонда (bfreeOrgan2)
 *        4    | Violin    — скрипка (волновод смычковой струны + резонансы деки)
 *        5... | SoundFont — по одной программе на КАЖДЫЙ пресет загруженного SF2 (5 = первый пресет банка,
 *             |             6 = второй и т.д.); на дисплее «SF:<имя пресета>»
 *
 * SoundFont стоит в конце списка: число его программ зависит от банка. Если банк не загружен, под номером 7
 * остаётся одна программа «SoundFont» (выбор покажет ошибку, а цикл Program Change 127 её пропускает).
 *
 * Номера программ вне диапазона заворачиваются по модулю SynthEngine_GetProgramCount()
 * (program % число программ), поэтому кнопки «+/−» на любой клавиатуре всегда выбирают
 * какой-нибудь инструмент, а не «молчат». Program Change 127 — «следующий инструмент» по кругу. Канал сообщения не учитывается (omni).
 *
 * Сам модуль выбор не сохраняет. В прошивке STM32 main.c сохраняет программу и громкость в резервные регистры RTC
 * (Platform/system/backup.h; для сохранения при выключении питания нужна батарейка на VBAT). Без батарейки или без
 * сохранённых данных после включения питания звучит E-Piano.
 *
 * Формат аудио: 48 кГц, signed 16 бит, стерео с чередованием (L, R, L, R...).
 *
 * Контекст вызовов (контракт одинаков для всех движков):
 *   SynthEngine_Init / NoteOn / NoteOff / ControlChange / PitchBend / ChannelPressure / PolyPressure /
 *   ProgramChange / Select
 *       — из основного цикла (main / MIDI-поток), только из ОДНОГО контекста;
 *   SynthEngine_FillStereoBuffer
 *       — из PendSV (STM32; DMA-прерывание только запрашивает рендер, см. Engine/audio/audio_halves.h)
 *         или из аудио-потока (ПК).
 *
 * Переключение движка не блокирует аудио-контекст: пока идёт смена,
 * FillStereoBuffer просто отдаёт тишину (см. synth_engine.cpp).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Порядок элементов = номер MIDI-программы (Program Change). */
typedef enum {
    SYNTH_ENGINE_EPIANO    = 0,
    SYNTH_ENGINE_PLUCK     = 1,
    SYNTH_ENGINE_LEAD      = 2,
    SYNTH_ENGINE_ORGAN     = 3,
    SYNTH_ENGINE_VIOLIN    = 4,
    SYNTH_ENGINE_SOUNDFONT = 5,   /* последним: за ним идут пресеты SF2 (см. SynthEngine_GetProgramCount) */
    SYNTH_ENGINE_COUNT
} SynthEngineId;

/*
 * Общая громкость с MIDI-клавиатуры: Control Change SYNTH_MASTER_VOLUME_CC (по умолчанию CC7 —
 * «Channel Volume»). Канал не учитывается.
 * У PCM5102A нет аппаратной регулировки громкости, поэтому коэффициент применяется программно
 * к выходному буферу в SynthEngine_FillStereoBuffer(), для любого движка. Кривая квадратичная
 * (уровень 127 -> 1.0, 64 -> ≈0.25, 0 -> тишина). Переопределяется через -D при сборке.
 */
#ifndef SYNTH_MASTER_VOLUME_CC
#define SYNTH_MASTER_VOLUME_CC 7
#endif

/* Уровень громкости после включения питания (0..127). 127 = без ослабления, как было до появления регулировки. */
#ifndef SYNTH_MASTER_VOLUME_DEFAULT
#define SYNTH_MASTER_VOLUME_DEFAULT 127
#endif

/*
 * Общая реверберация (Engine/synth/reverb.h): Control Change SYNTH_REVERB_CC (по умолчанию CC91 - «Reverb Send»), 0..127.
 * Как и громкость, это общая настройка прибора, а не движка: она не сбрасывается сменой инструмента и Panic().
 * В отличие от громкости она НЕ сохраняется при выключении питания. 0 = эффект выключен
 * (выход бит-в-бит как без него, процессор не тратится). Канал не учитывается; в движки CC91 не передаётся.
 * Кривая: усиление мокрого сигнала = SYNTH_REVERB_MAX_WET * (CC/127)^1.25. Уровень SYNTH_REVERB_DEFAULT (20 - значение
 * по умолчанию) даёт «немного».
 * Если времени на блок не хватает (движок занял больше SYNTH_REVERB_SHED_PCT бюджета), эффект плавно отключается раньше,
 * чем начнут гаситься голоса, и возвращается после паузы (см. ReverbLoadGuard).
 * SYNTH_REVERB_ENABLE=0 убирает эффект из сборки целиком (и 38 КБ ОЗУ).
 */
#ifndef SYNTH_REVERB_ENABLE
#define SYNTH_REVERB_ENABLE 1
#endif
#ifndef SYNTH_REVERB_CC
#define SYNTH_REVERB_CC 91
#endif
#ifndef SYNTH_REVERB_DEFAULT
#define SYNTH_REVERB_DEFAULT 20
#endif
#ifndef SYNTH_REVERB_MAX_WET
#define SYNTH_REVERB_MAX_WET 0.8f
#endif
#ifndef SYNTH_REVERB_SHED_PCT
#define SYNTH_REVERB_SHED_PCT 70      /* движок занял больше этой доли бюджета блока 2 блока подряд: реверберация выкл. */
#endif
#ifndef SYNTH_REVERB_RESTORE_PCT
#define SYNTH_REVERB_RESTORE_PCT 55   /* вернуть эффект, когда движок ниже этой доли бюджета ... */
#endif
#ifndef SYNTH_REVERB_RESTORE_BLOCKS
#define SYNTH_REVERB_RESTORE_BLOCKS 128   /* ... в течение стольких блоков подряд (около 0,34 с) */
#endif

/* Синтезатор, который звучит после включения питания. */
#define SYNTH_ENGINE_DEFAULT SYNTH_ENGINE_EPIANO

/** Сброс и включение синтезатора по умолчанию (E-Piano). Можно вызывать повторно. */
void SynthEngine_Init(void);

void SynthEngine_NoteOn(uint8_t midi_note, uint8_t velocity);   /* velocity 0 == NoteOff */
void SynthEngine_NoteOff(uint8_t midi_note);
/*
 * Control Change. Одинаково для всех движков (подробно - docs/ENGINES.md, «Единое поведение движков»):
 *   CC7   - общая громкость (в движки не передаётся);
 *   CC91  - общая реверберация (в движки не передаётся, см. SYNTH_REVERB_CC);
 *   CC120 - All Sound Off: движок быстро (10-20 мс) гасит все голоса, педаль сбрасывается, хвост реверберации гаснет за ~21 мс;
 *   CC123 - All Notes Off: выполняется здесь, в общем коде - Note Off по всем удерживаемым нотам и снятие педали;
 *   CC1   - мод-колесо: «живее, ярче»; в нуле звук не отличается от звука без колеса.
 * Остальные CC передаются активному движку.
 */
void SynthEngine_ControlChange(uint8_t control, uint8_t value);

/**
 * Колесо высоты тона (MIDI Pitch Bend): bend = -8192..+8191, 0 - центр (значения вне диапазона ограничиваются).
 * Последнее значение запоминается и применяется к новому движку при переключении (колесо - физический орган, оно
 * не сбрасывается сменой тембра); SynthEngine_Panic() возвращает его в центр. Движки без поддержки значение
 * игнорируют. Сейчас Pitch Bend обрабатывают E-Piano и Violin (±2 полутона) и SoundFont (по RPN 0, по умолчанию ±2).
 */
void SynthEngine_PitchBend(int16_t bend);

/**
 * Channel Pressure (монофонический Aftertouch), 0..127. Вызывает обработчик активного движка; сейчас только Violin использует
 * его (усиление смычка); остальные движки сообщение безвредно игнорируют.
 */
void SynthEngine_ChannelPressure(uint8_t pressure);

/**
 * Polyphonic Key Pressure (Aftertouch по нотам): note 0..127, pressure 0..127. Как и Channel Pressure, пока ни одним
 * движком не используется.
 */
void SynthEngine_PolyPressure(uint8_t midi_note, uint8_t pressure);

/**
 * MIDI Program Change: выбор синтезатора по номеру программы (program % SynthEngine_GetProgramCount()).
 * Если нужный синтезатор уже активен — ничего не происходит (звучащие ноты не обрываются).
 */
void SynthEngine_ProgramChange(uint8_t program);

/**
 * Явный выбор синтезатора. Новый движок стартует «с нуля» (все ноты сброшены).
 * @return 0, если id вне диапазона; иначе 1.
 */
int SynthEngine_Select(SynthEngineId id);

/** Число программ: SYNTH_ENGINE_SOUNDFONT программ встроенных движков + число пресетов SF2 (не меньше 1). */
uint32_t SynthEngine_GetProgramCount(void);

/** Номер текущей программы 0..GetProgramCount()-1 (для SoundFont — SYNTH_ENGINE_SOUNDFONT + индекс пресета). */
uint32_t SynthEngine_GetProgram(void);

/** Выбор программы по номеру (без заворачивания). @return 0, если номер вне диапазона. */
int SynthEngine_SelectProgram(uint32_t program);

/** Короткое имя программы для дисплея (ASCII, не длиннее out_size-1 символов): «Violin», «SF:Piano». */
void SynthEngine_GetProgramName(uint32_t program, char *out, uint32_t out_size);

/**
 * Переключение на следующий доступный синтезатор по циклу (как Program Change 127).
 * Пропускает недоступные движки (например, SoundFont без SF2).
 * @return Выбранный синтезатор.
 */
SynthEngineId SynthEngine_SelectNext(void);

/**
 * Можно ли сейчас выбрать синтезатор. Всегда 1, кроме SoundFont после неудачной загрузки
 * (нет SD-карты / файла SNDFNT.SF2 / не хватило памяти). Используется циклом Program Change 127.
 */
int SynthEngine_IsAvailable(SynthEngineId id);

/**
 * «Паника»: мгновенно гасит ВСЕ звучащие ноты активного синтезатора (его состояние сбрасывается
 * как при выборе). Нужна, когда Note Off уже не придут: USB-клавиатуру выдернули при зажатых клавишах.
 * Только из основного цикла (тот же контекст, что и NoteOn/Select). Активный движок не меняется. Колесо высоты
 * тона возвращается в центр (после отключения клавиатуры его положение неизвестно).
 */
void SynthEngine_Panic(void);

/** Общая громкость 0..127 (значения выше 127 ограничиваются). Можно вызывать из main. */
void SynthEngine_SetMasterVolume(uint8_t level);

/** Текущий уровень общей громкости 0..127. */
uint8_t SynthEngine_GetMasterVolume(void);

/** Глубина общей реверберации 0..127 (значения выше 127 ограничиваются); 0 - выключена. Можно вызывать из main. Не сохраняется. */
void SynthEngine_SetReverb(uint8_t level);

/** Текущая глубина реверберации 0..127 (не учитывает временное отключение защитой от перегрузки). */
uint8_t SynthEngine_GetReverb(void);

/** 1, если реверберация сейчас временно отключена защитой от перегрузки (движку не хватает времени на блок). */
int SynthEngine_IsReverbShed(void);

#if defined(PURRMIDI_HOST_BUILD) || defined(_WIN32) || defined(__unix__) || defined(__APPLE__)
/** Только для тестов на хосте: свои «часы» (такты) и бюджет блока из 128 кадров; now == NULL выключает защиту. */
void SynthEngine_SetClockForTest(uint32_t (*now)(void), uint32_t block_budget_cycles);
#endif

/** Текущий активный синтезатор. */
SynthEngineId SynthEngine_GetCurrent(void);

/** Имя синтезатора: "epiano" / "pluck" / "lead" / "organ" / "soundfont" (для логов). Для неверного id — "?". */
const char *SynthEngine_GetEngineName(SynthEngineId id);

/** Имя активного синтезатора. */
const char *SynthEngine_GetName(void);

/** Блочное заполнение стереобуфера (L, R, L, R...). */
void SynthEngine_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

#ifdef __cplusplus
}
#endif

#endif /* SYNTH_ENGINE_H */

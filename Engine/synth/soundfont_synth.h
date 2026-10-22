#ifndef SOUNDFONT_SYNTH_H
#define SOUNDFONT_SYNTH_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file    soundfont_synth.h
 * @brief   Синтезатор SoundFont2 на базе библиотеки TinySoundFont.
 *
 * Банк хранится только в QSPI Flash и читается на месте (XIP, 0x90000000); SD-карта используется лишь
 * при запуске для обновления банка (sf2_loader.h). На хосте банк — файл, прочитанный в ОЗУ.
 */

/* ---- Настройки микса SoundFont (можно переопределить через -D) ---- */
/* Master gain of the SoundFont mix, dB (0 = as in the bank). Many voices summed at 0 dB can exceed full
 * scale and clip hard; the clip counters in the [SF2] statistics show whether that happens. */
#ifndef SF2_GLOBAL_GAIN_DB
#define SF2_GLOBAL_GAIN_DB 0.0f
#endif

/* 1 = soft limiter on the mix. TinySoundFont sums all voices without headroom and hard-clips at full scale,
 * which sounds like harsh distortion as soon as a chord is louder than 0 dBFS (3-6 notes of a loud piano).
 * The limiter leaves everything below SF2_LIMITER_KNEE untouched and bends the peaks smoothly towards
 * full scale instead of cutting them off. 0 = old behaviour (hard clipping). */
#ifndef SF2_SOFT_LIMITER
#define SF2_SOFT_LIMITER 1
#endif
#ifndef SF2_LIMITER_KNEE
#define SF2_LIMITER_KNEE 0.6f     /* fraction of full scale where the limiter starts to act */
#endif
#ifndef SF2_LIMITER_CEILING
#define SF2_LIMITER_CEILING 0.98f /* the output never goes above this fraction of full scale */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Подготовка банка (QSPI, при необходимости обновление с SD, XIP) и инициализация SoundFont. true при успехе. */
bool SoundFontSynth_InitSF2(void);

/** Сброс голосов и состояния каналов. Безопасно вызывать из основного контекста (main). */
void SoundFontSynth_Init(void);

/** Обработка события Note On. */
void SoundFontSynth_NoteOn(uint8_t note, uint8_t velocity);

/** Обработка события Note Off. */
void SoundFontSynth_NoteOff(uint8_t note);

/** Обработка события MIDI Control Change. */
void SoundFontSynth_ControlChange(uint8_t control, uint8_t value);

/** Обработка MIDI Program Change: выбор пресета по индексу в списке банка (см. SoundFontSynth_SelectPreset). */
void SoundFontSynth_ProgramChange(uint8_t program);

/**
 * Колесо высоты тона: bend = -8192..+8191 (0 - центр). Диапазон задаётся RPN 0 (по умолчанию ±2 полутона).
 * Хранится как «последнее значение» (без очереди), поэтому возврат в центр не теряется при перегрузке.
 */
void SoundFontSynth_PitchBend(int16_t bend);

/**
 * Заполнение стереобуфера 48 кГц 16-бит PCM аудио.
 * Неблокирующий вызов для аудио-прерывания или аудио-потока.
 */
void SoundFontSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/** Число пресетов (инструментов) в загруженном банке SF2; 0, если банк не загружен. */
uint32_t SoundFontSynth_GetPresetCount(void);

/** Имя пресета по индексу 0..GetPresetCount()-1 (строка внутри банка, до 19 символов; NULL для неверного индекса). */
const char *SoundFontSynth_GetPresetName(uint32_t preset_index);

/**
 * Выбор пресета по индексу (порядок банка: по номеру банка и программы). Применяется в начале следующего аудиоблока;
 * уже звучащие ноты доигрывают прежним тембром, новые берут новый пресет.
 */
void SoundFontSynth_SelectPreset(uint32_t preset_index);

/** Индекс выбранного пресета. */
uint32_t SoundFontSynth_GetPreset(void);

/** Возвращает true, если банк SoundFont2 успешно загружен. */
bool SoundFontSynth_IsLoaded(void);

/** Сколько голосов TinySoundFont звучит сейчас (диагностика и тесты). */
uint32_t SoundFontSynth_GetActiveVoiceCount(void);

#if !defined(__arm__)
/**
 * Только для хоста (тесты): подменить счётчик тактов и задать бюджет блока из 128 кадров в тактах. По умолчанию на хосте
 * тактов нет, и регулятор нагрузки (откладывание Note On, гашение звучащих нот) выключен. now == NULL возвращает так.
 */
void SoundFontSynth_SetClockForTest(uint32_t (*now)(void), uint32_t block_budget_cycles);
#endif

/** Число 16-битных отсчётов PCM в банке (0, если банк не загружен). */
uint32_t SoundFontSynth_GetSampleCount(void);

/**
 * Проверка доступности синтезатора SoundFont для цикла переключения Program Change 127.
 * Возвращает false только если загрузка SoundFont уже вызвалась и завершилась ошибкой
 * (во флеше нет валидного банка и SD-карта с 0:/SNDFNT.SF2 не вставлена, либо не хватило памяти).
 * До первой попытки загрузки возвращает true (на хосте банк загружается лениво при первом выборе
 * движка; на прошивке загрузка выполняется при старте в main()).
 */
bool SoundFontSynth_IsAvailable(void);

/**
 * Статистика рендера SoundFont для UART. Собирается по каждому аудиоблоку (аудиоконтекст), печатается из
 * главного цикла. SoundFontSynth_ReportStats(now_ms) печатает не чаще раза в секунду и только пока играют ноты:
 *   [SF2] 1s: render avg/max (мкс, число блоков дольше бюджета) | voices max/now | notes, regions want/started/short
 *         | mix peak (1.00 FS = полная шкала), отсчёты выше 1.0 FS | fifo dropped
 *   - render: время SoundFontSynth_FillStereoBuffer(); бюджет блока из 128 кадров = 2666 мкс;
 *   - voices: активные голоса TinySoundFont (лимит 32); short = want - started: голоса, которых нотам не хватило
 *     (украдены у затухающих нот - щелчок - или потеряны);
 *   - mix peak: пик СУММЫ голосов до лимитера (1.00 FS = 0 dBFS); отсчёты выше 1.0 FS без лимитера были бы жёстко
 *     обрезаны (искажения). Мягкий лимитер включён по умолчанию (SF2_SOFT_LIMITER), порог SF2_LIMITER_KNEE;
 *   - fifo dropped: потерянные MIDI-события (очередь в аудиоконтекст переполнена).
 * Окно статистики сбрасывается после каждой печати.
 */
void SoundFontSynth_ReportStats(uint32_t now_ms);
void SoundFontSynth_ResetStats(void);

#ifdef __cplusplus
}
#endif

#endif /* SOUNDFONT_SYNTH_H */

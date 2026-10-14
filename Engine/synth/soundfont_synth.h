#ifndef SOUNDFONT_SYNTH_H
#define SOUNDFONT_SYNTH_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file    soundfont_synth.h
 * @brief   Синтезатор SoundFont2 на базе библиотеки TinySoundFont и кэша сэмплов SF2Cache.
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

/** Загрузка файла 0:/SNDFNT.SF2 с SD-карты и инициализация SoundFont. Возвращает true при успехе. */
bool SoundFontSynth_InitSF2(void);

/** Сброс голосов и состояния каналов. Безопасно вызывать из основного контекста (main). */
void SoundFontSynth_Init(void);

/** Обработка события Note On. */
void SoundFontSynth_NoteOn(uint8_t note, uint8_t velocity);

/** Обработка события Note Off. */
void SoundFontSynth_NoteOff(uint8_t note);

/** Обработка события MIDI Control Change. */
void SoundFontSynth_ControlChange(uint8_t control, uint8_t value);

/** Обработка события MIDI Program Change. */
void SoundFontSynth_ProgramChange(uint8_t program);

/**
 * Заполнение стереобуфера 48 кГц 16-бит PCM аудио.
 * Неблокирующий вызов для аудио-прерывания или аудио-потока.
 */
void SoundFontSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/** Фоновая обработка запросов к кэшу SF2 с SD-карты. Вызывается из основного цикла. */
void SoundFontSynth_Process(void);

/** Возвращает true, если файл SoundFont2 успешно загружен. */
bool SoundFontSynth_IsLoaded(void);

/**
 * Проверка доступности синтезатора SoundFont для цикла переключения Program Change 127.
 * Возвращает false только если загрузка SoundFont уже вызвалась и завершилась ошибкой
 * (отсутствует SD-карта, нет файла 0:/SNDFNT.SF2 или не хватило памяти). До первой
 * попытки загрузки возвращает true, так как файл загружается лениво (lazy load)
 * при первом явном выборе движка.
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

/**
 * Только для прошивки STM32: диагностика при запуске системы (до старта аудио DMA, строки [SF2DIAG] и [XIP]):
 * число голосов на ноту, конфигурация XIP (D-cache, MPU, регистры QUADSPI, частота QSPI), скорость чтения банка
 * из XIP без синтезатора и стоимость аккордов из 1/3/6/10 нот в реальном движке при включённом и выключенном
 * D-cache. Пустая заглушка в хостовой сборке.
 */
void SoundFontSynth_Profile(void);

#ifdef __cplusplus
}
#endif

#endif /* SOUNDFONT_SYNTH_H */

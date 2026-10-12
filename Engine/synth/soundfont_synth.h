#ifndef SOUNDFONT_SYNTH_H
#define SOUNDFONT_SYNTH_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file    soundfont_synth.h
 * @brief   Синтезатор SoundFont2 на базе библиотеки TinySoundFont и кэша SF2Cache.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Загрузка файла 0:/SNDFNT.SF2 и инициализация движка SoundFont. Возвращает true при успехе. */
bool SoundFontSynth_InitSF2(void);

/** Сброс голосов и состояния каналов синтезатора. Безопасно вызывать из основного цикла. */
void SoundFontSynth_Init(void);

/** Обработка события Note On. */
void SoundFontSynth_NoteOn(uint8_t note, uint8_t velocity);

/** Обработка события Note Off. */
void SoundFontSynth_NoteOff(uint8_t note);

/** Обработка MIDI Control Change. */
void SoundFontSynth_ControlChange(uint8_t control, uint8_t value);

/** Обработка MIDI Program Change. */
void SoundFontSynth_ProgramChange(uint8_t program);

/** Заполнение стереобуфера 48 кГц 16-бит PCM аудио (неблокирующий вызов). */
void SoundFontSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames);

/** Фоновая обработка запросов к кэшу SF2 с SD-карты. Вызывается из основного цикла. */
void SoundFontSynth_Process(void);

/** Возвращает true, если файл SoundFont2 успешно загружен в память. */
bool SoundFontSynth_IsLoaded(void);

/**
 * Проверка доступности синтезатора SoundFont.
 * Возвращает false только в случае, если попытка загрузки файла SF2 уже была сделана
 * и завершилась ошибкой (нет SD-карты, нет файла SNDFNT.SF2, нехватка памяти).
 */
bool SoundFontSynth_IsAvailable(void);

/** Вывод статистики работы кэша (попадания, промахи, времена чтения SD) в UART/stderr. */
void SoundFontSynth_PrintStats(void);

/**
 * Профилирование производительности (только для STM32): измеряет время атомарных
 * операций и работы кэша SF2Cache_GetSample(). Вызывается до старта аудио DMA.
 */
void SoundFontSynth_Profile(void);

#ifdef __cplusplus
}
#endif

#endif /* SOUNDFONT_SYNTH_H */

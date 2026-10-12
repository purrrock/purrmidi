#ifndef SOUNDFONT_SYNTH_H
#define SOUNDFONT_SYNTH_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file    soundfont_synth.h
 * @brief   Синтезатор SoundFont2 на базе библиотеки TinySoundFont и кэша сэмплов SF2Cache.
 */

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

/** Вывод одной строки со статистикой кэша (попадания, промахи, такты, время чтения SD) в stderr (UART). */
void SoundFontSynth_PrintStats(void);

/**
 * Только для прошивки STM32: профилирование при запуске системы. Измеряет стоимость
 * атомарных операций, работы SF2Cache_GetSample() (попадания по подсказке/поиску, промахи)
 * и симулирует аккорд с фоновым обслуживанием кэша в главном цикле.
 * Должен запускаться до старта аудио DMA. Выводит строки [SF2PROF]. Пустая заглушка в хостовой сборке.
 */
void SoundFontSynth_Profile(void);

#ifdef __cplusplus
}
#endif

#endif /* SOUNDFONT_SYNTH_H */

#ifndef SF2_CACHE_H
#define SF2_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "tsf.h"

/**
 * @file    sf2_cache.h
 * @brief   Ограниченный RAM-кэш сэмплов PCM и файловый бэкенд для TinySoundFont.
 *
 * Потоковая модель
 * ----------------
 *  - Аудио-контекст (DMA ISR на STM32, аудио-поток на ПК) только ЧИТАЕТ кэш через
 *    SF2Cache_GetSample() и никогда не обращается к файлу напрямую.
 *    Промах кэша возвращает тишину и ставит блок в очередь на загрузку.
 *  - Основной контекст выполнят ВСЕ файловые операции ввода-вывода: SF2Cache_ProcessRequests()
 *    и SF2Cache_PreloadBlock(). Они должны вызываться из одного (главного) потока.
 *
 * Почему кэш читает с опережением (read-ahead)
 * --------------------------------------------
 * Реактивный кэш (загрузка блока только после промаха в аудиопотоке) неизбежно приводит к
 * щелчкам и выпадению звука, так как голос продолжает воспроизведение во время чтения SD-карты.
 * По этой причине:
 *   1. SF2Cache_PreloadBlock() загружает начальные блоки ноты ДО её старта;
 *   2. Каждый читаемый аудиопотоком блок помечается, и главный цикл подгружает
 *      следующие SF2_READAHEAD_BLOCKS блоков;
 *   3. SF2Cache_ProcessRequests() обрабатывает до SF2_MAX_BLOCKS_PER_PROCESS блоков за вызов.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* На хосте (Windows/Linux) используется stdio; на целевой прошивке — FatFs. */
#ifndef SF2_HOST_MODE
#if defined(PURRMIDI_HOST_BUILD) || defined(_WIN32) || defined(WIN32) || defined(__unix__) || defined(__APPLE__)
#define SF2_HOST_MODE 1
#else
#define SF2_HOST_MODE 0
#endif
#endif

#define SF2_BLOCK_SAMPLES 1024U    /* 1024 отсчёта int16_t = 2048 байт на блок кэша */

/*
 * Количество блоков кэша:
 *  - Прошивка: 32 блока x 2 КБ = 64 КБ ОЗУ (строго ограничено).
 *  - Хост:     256 блоков x 2 КБ = 512 КБ.
 */
#ifndef SF2_CACHE_BLOCKS
#if SF2_HOST_MODE
#define SF2_CACHE_BLOCKS  256U
#else
#define SF2_CACHE_BLOCKS  32U
#endif
#endif

/* Количество блоков, загружаемых с опережением (read-ahead) */
#ifndef SF2_READAHEAD_BLOCKS
#define SF2_READAHEAD_BLOCKS 3U
#endif

/* Максимальное количество блоков, считываемых за один вызов SF2Cache_ProcessRequests() */
#ifndef SF2_MAX_BLOCKS_PER_PROCESS
#if SF2_HOST_MODE
#define SF2_MAX_BLOCKS_PER_PROCESS 64U
#else
#define SF2_MAX_BLOCKS_PER_PROCESS 2U
#endif
#endif

typedef enum {
    SF2_BLOCK_EMPTY    = 0,
    SF2_BLOCK_LOADING  = 1,
    SF2_BLOCK_READY    = 2,
    SF2_BLOCK_EVICTING = 3
} SF2BlockState;

/** Диагностические счётчики работы кэша. */
typedef struct {
    uint32_t miss_samples;     /**< Количество отсчётов, заменённых тишиной при промахах */
    uint32_t blocks_loaded;    /**< Успешно загруженные блоки */
    uint32_t load_errors;      /**< Ошибки чтения блоков с носителя */
} SF2CacheStats;

/**
 * Профилирование производительности (такты процессора DWT на прошивке, 0 на ПК).
 */
typedef struct {
    uint32_t calls;              /**< Вызовы SF2Cache_GetSample() */
    uint32_t hint_hits;          /**< Попадания по подсказке (remembered slot) */
    uint32_t search_hits;        /**< Попадания в результате линейного поиска */
    uint32_t misses;             /**< Промахи (возврат тишины и запуск подгрузки) */
    uint32_t search_slots;       /**< Просмотрено слотов при поиске */
    uint32_t cycles_hint;        /**< Суммарные такты на попадания по подсказке */
    uint32_t cycles_search;      /**< Суммарные такты на поиски */
    uint32_t cycles_miss;        /**< Суммарные такты на обработку промахов */
    uint32_t max_cycles;         /**< Максимальное время одного вызова */
    uint32_t proc_calls;         /**< Вызовы SF2Cache_ProcessRequests() */
    uint32_t proc_cycles_total;
    uint32_t proc_cycles_max;
    uint32_t read_calls;         /**< Операции чтения блоков с носителя */
    uint32_t read_cycles_total;
    uint32_t read_cycles_max;
} SF2Profile;

void SF2Cache_GetProfile(SF2Profile *out);

/**
 * Вызывается в начале каждого аудио-блока перед первым SF2Cache_GetSample().
 * Продвигают часы LRU и сбрасывают кэш «известных промахов».
 */
void SF2Cache_BeginAudioBlock(void);
void SF2Cache_ResetProfile(void);

/** Открытие файла SF2 с помощью бэкенда платформы (FatFs / stdio). */
bool SF2Cache_OpenFile(const char *filepath);

/** Закрытие открытого файла и сброс состояния кэша. */
void SF2Cache_CloseFile(void);

/** Возвращает true, если файл SF2 открыт. */
bool SF2Cache_IsFileOpen(void);

/** Сброс блоков кэша и очереди запросов. */
void SF2Cache_Reset(void);

/** Настройка колбэков tsf_stream для чтения метаданных SoundFont. */
void SF2Cache_InitStream(struct tsf_stream *stream);

/**
 * Обработчик запросов в главном цикле: загружает пропущенные аудиопотоком блоки
 * и выполняет упреждающее чтение (read-ahead).
 */
void SF2Cache_ProcessRequests(void);

/**
 * Синхронная загрузка одного блока (только главный контекст).
 * Используется перед стартом ноты для предзагрузки первых блоков.
 */
bool SF2Cache_PreloadBlock(uint32_t block_index);

/** Запрос загрузки блока в кэш (вызывается аудиопотоком при промахе). */
void SF2Cache_RequestBlock(uint32_t block_index);

/** Выборка 16-битного отсчёта PCM для sample_index (неблокирующий вызов). */
short SF2Cache_GetSample(tsf *f, uint32_t sample_index, int *last_slot_hint);

/** Получение текущего смещения чанка smpl в файле SF2. */
uint32_t SF2Cache_GetSmplFileOffset(void);

/** Установка смещения данных smpl в файле. */
void SF2Cache_SetSmplFileOffset(uint32_t file_offset, uint32_t total_samples);

/** Копирование диагностических счётчиков. */
void SF2Cache_GetStats(SF2CacheStats *out);

/** Обнуление диагностических счётчиков. */
void SF2Cache_ResetStats(void);

#ifdef __cplusplus
}
#endif

#endif /* SF2_CACHE_H */

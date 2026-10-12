#ifndef SF2_CACHE_H
#define SF2_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "tsf.h"

/**
 * @file    sf2_cache.h
 * @brief   Ограниченный RAM-кэш PCM-сэмплов и файловый бэкенд для TinySoundFont.
 *
 * Потоковая модель
 * ----------------
 *  - Аудиоконтекст (прерывание DMA ISR на STM32, поток аудио-callback на ПК) только ЧИТАЕТ кэш через
 *    SF2Cache_GetSample() и никогда напрямую не касается файла. Промах возвращает тишину и ставит запрос в очередь.
 *  - Главный контекст (main) владеет ВСЕМ файловым вводом-выводом: SF2Cache_ProcessRequests() и SF2Cache_PreloadBlock().
 *    Они должны вызываться строго из одного (главного) потока и никогда не параллельно друг с другом.
 *
 * Почему кэш должен читать с опережением (read-ahead)
 * --------------------------------------------------
 * Чисто реактивный кэш (загрузка блока только после того, как аудиопоток допустил промах) не может работать без артефактов:
 * голос продолжает воспроизведение, пока загружается блок, поэтому каждая граница блока превращается в паузу тишины
 * (слышимую как щелчки/шум). Если главный цикл обрабатывает данные медленнее, чем их потребляют голоса, звук исчезает полностью.
 * По этой причине:
 *   1. SF2Cache_PreloadBlock() позволяет синтезатору загрузить первые блоки ноты ДО её старта;
 *   2. Каждый блок, считываемый аудиопотоком, помечается как использованный ("touched"), и главный цикл подгружает
 *      следующие SF2_READAHEAD_BLOCKS блоков;
 *   3. SF2Cache_ProcessRequests() обрабатывает до SF2_MAX_BLOCKS_PER_PROCESS блоков за один вызов.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Сборка для хоста (тесты Windows / Linux / macOS) использует stdio; целевая прошивка — FatFs. */
#ifndef SF2_HOST_MODE
#if defined(PURRMIDI_HOST_BUILD) || defined(_WIN32) || defined(WIN32) || defined(__unix__) || defined(__APPLE__)
#define SF2_HOST_MODE 1
#else
#define SF2_HOST_MODE 0
#endif
#endif

#define SF2_BLOCK_SAMPLES 1024U    /* 1024 отсчётов int16_t = 2048 байт на один блок кэша */

/*
 * Количество блоков кэша:
 *  - Прошивка: 32 блока x 2 КБ = 64 КБ ОЗУ (строго ограничено архитектурой).
 *  - Хост:     256 блоков x 2 КБ = 512 КБ. Зацикленная пианино-нота удерживает ~14 блоков петли постоянными
 *              на один сэмпл (L и R), поэтому 32 блока исчерпываются парой нот.
 */
#ifndef SF2_CACHE_BLOCKS
#if SF2_HOST_MODE
#define SF2_CACHE_BLOCKS  256U
#else
#define SF2_CACHE_BLOCKS  32U
#endif
#endif

/* Сколько блоков после читаемого аудиопотоком блока загружается упреждающе. */
#ifndef SF2_READAHEAD_BLOCKS
#define SF2_READAHEAD_BLOCKS 3U
#endif

/* Верхняя граница считываний из файла за один вызов SF2Cache_ProcessRequests(). */
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

/** Диагностические счётчики (монотонные атомики с relaxed-упорядочиванием). */
typedef struct {
    uint32_t miss_samples;     /**< Количество отсчётов, заменённых тишиной в аудиопотоке */
    uint32_t blocks_loaded;    /**< Блоки, успешно прочитанные из файла */
    uint32_t load_errors;      /**< Сбои или незавершённые (короткие) чтения блоков */
} SF2CacheStats;

/**
 * Диагностика профилирования (число тактов CPU по DWT-счётчику на прошивке, 0 для хост-сборки).
 * Заполняется из аудиопрерывания (GetSample) и главного цикла (ProcessRequests / чтения).
 */
typedef struct {
    uint32_t calls;              /**< Вызовы SF2Cache_GetSample(), дошедшие до кэша */
    uint32_t hint_hits;          /**< Попадания из запомненного слота (hint) голоса */
    uint32_t search_hits;        /**< Попадания после линейного поиска по слотам */
    uint32_t misses;             /**< Блок не в ОЗУ: возвращена тишина и затребованы блоки */
    uint32_t search_slots;       /**< Проверено слотов при всех линейных поисках (hits и misses) */
    uint32_t cycles_hint;        /**< Суммарные такты, затраченные на попадания по подсказке */
    uint32_t cycles_search;      /**< Суммарные такты, затраченные на линейный поиск */
    uint32_t cycles_miss;        /**< Суммарные такты на промахи (включая два вызова RequestBlock) */
    uint32_t max_cycles;         /**< Худшее время выполнения одного вызова */
    uint32_t proc_calls;         /**< Количество вызовов SF2Cache_ProcessRequests() */
    uint32_t proc_cycles_total;  /**< Суммарные такты в ProcessRequests() */
    uint32_t proc_cycles_max;    /**< Максимальные такты в ProcessRequests() */
    uint32_t read_calls;         /**< Вызовы чтения блоков с носителя */
    uint32_t read_cycles_total;  /**< Суммарные такты чтения с носителя */
    uint32_t read_cycles_max;    /**< Максимальные такты чтения одного блока */
} SF2Profile;

void SF2Cache_GetProfile(SF2Profile *out);

/**
 * Вызывается один раз в начале каждого аудио-блока перед первым SF2Cache_GetSample() этого блока.
 * Продвигают часы LRU и сбрасывают память «известных промахов» данного блока.
 */
void SF2Cache_BeginAudioBlock(void);
void SF2Cache_ResetProfile(void);

/** Открытие SF2-файла с помощью файлового бэкенда платформы (FatFs на прошивке, stdio на хосте). */
bool SF2Cache_OpenFile(const char *filepath);

/** Закрытие открытого файла и сброс состояния кэша. */
void SF2Cache_CloseFile(void);

/** Возвращает true, если SF2-файл открыт в данный момент. */
bool SF2Cache_IsFileOpen(void);

/** Сброс блоков кэша и очереди запросов. */
void SF2Cache_Reset(void);

/** Настройка колбэков tsf_stream для чтения метаданных SoundFont. */
void SF2Cache_InitStream(struct tsf_stream *stream);

/**
 * Сервисная процедура главного цикла: загружает блоки, пропущенные аудиопотоком, затем читает с опережением
 * блоки, которые сейчас воспроизводятся. Выполняет не более SF2_MAX_BLOCKS_PER_PROCESS считываний за вызов.
 * Блок, который не удалось загрузить, повторяет попытку при СЛЕДУЮЩЕМ вызове (никогда внутри того же вызова).
 */
void SF2Cache_ProcessRequests(void);

/**
 * Синхронная загрузка одного блока (только в контексте main). Используется синтезатором для загрузки
 * первых блоков ноты в ОЗУ ДО старта голоса. Возвращает true, если блок находится в кэше при возврате.
 */
bool SF2Cache_PreloadBlock(uint32_t block_index);

/** Запрос на загрузку блока в кэш. Вызывается аудиопотоком при промахе. */
void SF2Cache_RequestBlock(uint32_t block_index);

/** Выборка 16-битного отсчёта PCM для sample_index. Неблокирующий вызов из аудиопотока. */
short SF2Cache_GetSample(tsf *f, uint32_t sample_index, int *last_slot_hint);

/** Получение текущего смещения чанка smpl в файле SF2. */
uint32_t SF2Cache_GetSmplFileOffset(void);

/** Установка байтового смещения данных smpl в файле. */
void SF2Cache_SetSmplFileOffset(uint32_t file_offset, uint32_t total_samples);

/** Копирование диагностических счётчиков. */
void SF2Cache_GetStats(SF2CacheStats *out);

/** Сброс диагностических счётчиков. */
void SF2Cache_ResetStats(void);

#ifdef __cplusplus
}
#endif

#endif /* SF2_CACHE_H */

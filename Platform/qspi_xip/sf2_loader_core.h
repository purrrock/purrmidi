/**
 * @file    sf2_loader_core.h
 * @brief   Логика обновления SoundFont-банка во внешней QSPI Flash (без привязки к HAL/FatFs).
 *
 * Ядро работает через две таблицы операций: SF2FlashOps (флеш) и SF2SourceOps (источник файла,
 * на прошивке - SD-карта через FatFs). Благодаря этому вся логика проверяется на хосте
 * (tests/test_sf2_loader_core.cpp) с эмуляцией NOR-флеша, включая сбой питания посреди записи.
 *
 * Раскладка флеша (flash_size = 8 МБ):
 *
 *   0x000000 ... bank_size-1     банк SF2 как есть (XIP: 0x90000000 + смещение);
 *   ...      ... 0x7FEFFF        свободно, не стирается;
 *   0x7FF000 ... 0x7FFFFF        сектор заголовка SF2L_Header (последние 4 КБ).
 *
 * Максимальный размер банка = flash_size - 4096 (для 8 МБ: 8 384 512 байт).
 *
 * Порядок обновления (защита от потери питания и от лишнего износа):
 *   1. заголовок валиден и совпадает с файлом (размер + штамп времени) -> флеш не трогаем;
 *   2. штамп другой -> считаем CRC32 файла; совпал с заголовком -> флеш не трогаем;
 *   3. иначе: стереть сектор заголовка (банк становится "невалидным"), стереть и записать данные,
 *      проверить CRC32 через XIP (тем же путём, которым потом читает синтезатор),
 *      и только потом записать заголовок. Сбой в любой момент -> заголовка нет -> при следующем
 *      старте запись повторится.
 */
#ifndef SF2_LOADER_CORE_H
#define SF2_LOADER_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF2L_PAGE_SIZE      256u
#define SF2L_SECTOR_SIZE    4096u
#define SF2L_BLOCK64_SIZE   65536u
#define SF2L_BANK_MIN_SIZE  1024u
#define SF2L_MAGIC          0x46534D50u   /* "PMSF" (little-endian) */
#define SF2L_LAYOUT         1u

/** Заголовок банка, лежит в последнем секторе флеша (64 байта, остальное в секторе 0xFF). */
typedef struct {
    uint32_t magic;          /* SF2L_MAGIC                                             */
    uint32_t layout;         /* SF2L_LAYOUT                                            */
    uint32_t bank_size;      /* размер банка в байтах                                  */
    uint32_t bank_crc32;     /* CRC32 всего банка                                      */
    uint32_t src_stamp;      /* штамп источника: (FAT date << 16) | FAT time            */
    uint32_t reserved[10];   /* 0xFFFFFFFF                                             */
    uint32_t header_crc32;   /* CRC32 предыдущих 60 байт                               */
} SF2L_Header;

typedef enum { SF2SRC_OK = 0, SF2SRC_NO_MEDIA, SF2SRC_NO_FILE, SF2SRC_ERROR } SF2SrcStatus;

/** Источник файла (SD-карта). read() - последовательное чтение; rewind() - в начало. */
typedef struct {
    SF2SrcStatus (*open)(void *ctx, uint32_t *size, uint32_t *stamp);
    bool         (*read)(void *ctx, void *dst, uint32_t len, uint32_t *got);
    bool         (*rewind)(void *ctx);
    void         (*close)(void *ctx);
} SF2SourceOps;

/** Операции флеша. Все адреса - смещения от начала флеша (не 0x90000000). */
typedef struct {
    uint32_t flash_size;
    bool (*read)(void *ctx, uint32_t addr, void *dst, uint32_t len);          /* indirect-чтение     */
    bool (*erase_sector)(void *ctx, uint32_t addr);                           /* 4 КБ                */
    bool (*erase_block64)(void *ctx, uint32_t addr);                          /* 64 КБ               */
    bool (*program_page)(void *ctx, uint32_t addr, const void *src, uint32_t len); /* в пределах страницы 256 Б */
    bool (*map)(void *ctx, const uint8_t **base);                             /* включить XIP        */
    void (*unmap)(void *ctx);                                                 /* выключить XIP       */
} SF2FlashOps;

typedef enum { SF2L_PHASE_CHECK = 0, SF2L_PHASE_ERASE, SF2L_PHASE_WRITE, SF2L_PHASE_VERIFY } SF2L_Phase;
typedef void (*SF2L_ProgressFn)(void *ctx, SF2L_Phase phase, uint32_t done, uint32_t total);

typedef enum {
    SF2L_OK_UNCHANGED = 0,   /* во флеше актуальный банк (SD нет / файла нет / файл тот же)  */
    SF2L_OK_UPDATED,         /* банк записан из файла и проверен                              */
    SF2L_ERR_PARAM,
    SF2L_ERR_NO_BANK,        /* во флеше нет валидного банка и взять его неоткуда             */
    SF2L_ERR_WRITE,          /* ошибка стирания/записи (банк во флеше невалиден)              */
    SF2L_ERR_VERIFY,         /* CRC после записи не совпал (банк во флеше невалиден)          */
    SF2L_ERR_FLASH           /* не удалось включить XIP                                       */
} SF2L_Result;

typedef enum {
    SF2L_FILE_NOT_CHECKED = 0,
    SF2L_FILE_NO_MEDIA,      /* карты нет                                     */
    SF2L_FILE_NO_FILE,       /* на карте нет SNDFNT.SF2                       */
    SF2L_FILE_READ_ERROR,
    SF2L_FILE_TOO_SMALL,
    SF2L_FILE_TOO_LARGE,     /* больше flash_size - 4096                      */
    SF2L_FILE_BAD_SIGNATURE, /* не RIFF/sfbk или размер RIFF больше файла     */
    SF2L_FILE_SAME,          /* совпадает с банком во флеше                   */
    SF2L_FILE_NEW            /* новый/изменённый, записан во флеш             */
} SF2L_FileStatus;

typedef struct {
    SF2L_Result     result;
    SF2L_FileStatus file_status;
    const uint8_t  *base;      /* XIP-указатель на банк (только при OK_*) */
    uint32_t        size;
    uint32_t        crc32;
} SF2L_Report;

typedef struct {
    const SF2FlashOps  *flash;   void *flash_ctx;
    const SF2SourceOps *src;     void *src_ctx;     /* src может быть NULL: только XIP */
    uint8_t            *buf;     uint32_t buf_size; /* буфер обмена, кратен 256, >= 256 */
    SF2L_ProgressFn     progress; void *progress_ctx;
} SF2L_Env;

/** Выполняет весь сценарий: проверка -> (запись) -> XIP. */
SF2L_Result SF2L_Run(const SF2L_Env *env, SF2L_Report *report);

/** Проверка сигнатуры SF2 по первым 12 байтам и размеру файла/банка. */
bool SF2L_SignatureOk(const uint8_t head12[12], uint32_t total_size);

const char *SF2L_ResultName(SF2L_Result r);
const char *SF2L_FileStatusName(SF2L_FileStatus s);

#ifdef __cplusplus
}
#endif

#endif /* SF2_LOADER_CORE_H */

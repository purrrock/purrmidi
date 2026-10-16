/**
 * @file    sd_storage.h
 * @brief   microSD (SDMMC1) + FatFs для PurrMidi - только чтение, только при запуске.
 *
 * SD-карта используется лишь для обновления SoundFont-банка в QSPI Flash (sf2_loader.c): карту
 * монтируют при старте, читают 0:/SNDFNT.SF2 и размонтируют. Синтезатор карту не использует.
 *
 * Модуль решает проблему "DTCM и DMA": IDMA контроллера SDMMC1 не видит DTCM
 * (там лежат .data/.bss/стек/куча), а FatFs передаёт драйверу диска указатели
 * на внутренние буферы объектов FATFS и FIL. Диск-драйвер из этого модуля
 * сам проверяет адрес буфера и, если он не в AXI SRAM, читает через
 * промежуточный (bounce) буфер из секции .dma_buffer. Поэтому FATFS, FIL и
 * пользовательские буферы можно держать в любой памяти.
 *
 * Модуль не зависит от кода, который CubeMX генерирует для SD-диск-драйвера,
 * поэтому в CubeMX нужен режим FATFS "User-defined" (см. README).
 */
#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SDS_OK = 0,        /* карта инициализирована и смонтирована */
    SDS_ERR_NO_CARD,   /* датчик SD_DETECT показывает, что карты нет */
    SDS_ERR_INIT,      /* HAL_SD_Init / переключение на 4 бита не удалось */
    SDS_ERR_LINK,      /* FATFS_LinkDriver не удался (см. "Number of volumes" и MX_FATFS_Init) */
    SDS_ERR_MOUNT      /* f_mount не удался (нет файловой системы FAT/exFAT?) */
} SDStorage_Result;

/** Инициализирует SDMMC1, подключает диск-драйвер и монтирует том.
 *  Безопасно вызывать повторно (например, после вставки карты). */
SDStorage_Result SDStorage_Mount(void);

/** Диагностика последней попытки SDStorage_Mount(): на каком этапе и почему не получилось. */
typedef struct {
    SDStorage_Result result;   /* итог последнего SDStorage_Mount() */
    int      detect_level;     /* сырой уровень SD_DETECT (0/1), -1 если пин не назван SD_DETECT */
    int      hal_init;         /* результат HAL_SD_Init (HAL_OK=0, HAL_ERROR=1, ...), -1 если не вызывался */
    int      hal_wide;         /* результат HAL_SD_ConfigWideBusOperation, -1 если не вызывался */
    uint32_t hal_error;        /* hsd1.ErrorCode (маска HAL_SD_ERROR_*, 0 = нет ошибки) */
    int      link_ret;         /* результат FATFS_LinkDriver (0 = успех), -1 если не вызывался */
    int      f_mount_res;      /* FRESULT от f_mount, -1 если не вызывался */
} SDStorage_Diag;

const SDStorage_Diag *SDStorage_GetDiag(void);

/** Короткое имя результата для логов. */
const char *SDStorage_ResultName(SDStorage_Result r);

/** Печатает SDStorage_Diag в stderr (UART). */
void SDStorage_PrintDiag(void);

/** Печатает в stderr содержимое корня тома (до 24 записей): видно, как файл называется на самом деле. */
void SDStorage_ListRoot(void);

/** Размонтирует том и выключает SDMMC1. */
void SDStorage_Unmount(void);

bool SDStorage_IsMounted(void);

/** true, если карта вставлена. Если в CubeMX пин PD4 не назван SD_DETECT, всегда true. */
bool SDStorage_CardPresent(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_STORAGE_H */

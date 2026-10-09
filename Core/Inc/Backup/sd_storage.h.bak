/**
 * @file    sd_storage.h
 * @brief   microSD (SDMMC1) + FatFs для PurrMidi.
 *
 * Модуль решает проблему "DTCM и DMA": IDMA контроллера SDMMC1 не видит DTCM
 * (там лежат .data/.bss/стек/куча), а FatFs передаёт драйверу диска указатели
 * на внутренние буферы объектов FATFS и FIL. Диск-драйвер из этого модуля
 * сам проверяет адрес буфера и, если он не в AXI SRAM, читает/пишет через
 * промежуточный (bounce) буфер из секции .dma_buffer. Поэтому FATFS, FIL и
 * пользовательские буферы можно держать в любой памяти.
 *
 * Модуль не зависит от кода, который CubeMX генерирует для SD-диск-драйвера,
 * поэтому в CubeMX нужен режим FATFS "User-defined" (см. README).
 */
#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include <stdbool.h>

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

/** Размонтирует том и выключает SDMMC1. */
void SDStorage_Unmount(void);

bool SDStorage_IsMounted(void);

/** Путь тома для f_open() и т.п. (обычно "0:/"). */
const char *SDStorage_Path(void);

/** true, если карта вставлена. Если в CubeMX пин PD4 не назван SD_DETECT, всегда true. */
bool SDStorage_CardPresent(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_STORAGE_H */

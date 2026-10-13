/**
 * @file    qspi_flash.h
 * @brief   Драйвер внешней QSPI Flash (Winbond W25Q64, 8 МБ, WeAct STM32H743 Mini) на HAL_QSPI.
 *
 * Два режима:
 *   - indirect: чтение (0xEB), стирание 4 КБ / 64 КБ, запись страниц по 256 Б;
 *   - memory-mapped (XIP): флеш отображается в 0x90000000 только для чтения (команда 0xEB, 4 линии).
 *
 * Требования к CubeMX: см. docs/SOUNDFONT_QSPI_SYNTH.md (QUADSPI, Bank1, 4 линии, Flash Size 22 бита, генерация quadspi.c).
 * Драйвер вызывает MX_QUADSPI_Init() сам, если CubeMX не сделал это раньше.
 * Для других микросхем (не Winbond) нужно проверить коды команд и число dummy-циклов.
 */
#ifndef QSPI_FLASH_H
#define QSPI_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef QSPIFLASH_XIP_BASE
#define QSPIFLASH_XIP_BASE        0x90000000UL
#endif

#ifndef QSPIFLASH_SIZE
#define QSPIFLASH_SIZE            0x800000UL      /* 8 МБ */
#endif

/* Dummy-циклы команды 0xEB. W25Q64: 2 такта режимного байта (передаётся как AlternateBytes) + 4 dummy. */
#ifndef QSPIFLASH_READ_DUMMY_CYCLES
#define QSPIFLASH_READ_DUMMY_CYCLES  4U
#endif

/** Сброс флеша, чтение JEDEC ID, проверка ёмкости, включение Quad Enable (бит QE в SR2). */
bool QSPIFlash_Init(void);

/** JEDEC ID (производитель, тип, ёмкость), заполняется в QSPIFlash_Init(). */
void QSPIFlash_GetJedecId(uint8_t id[3]);

/* Indirect-режим. Если включён XIP, функции сами из него выходят. Адреса - смещения от начала флеша. */
bool QSPIFlash_Read(uint32_t addr, void *dst, uint32_t len);
bool QSPIFlash_EraseSector(uint32_t addr);                         /* 4 КБ, addr кратен 4096   */
bool QSPIFlash_EraseBlock64(uint32_t addr);                        /* 64 КБ, addr кратен 65536 */
bool QSPIFlash_ProgramPage(uint32_t addr, const void *src, uint32_t len);   /* len <= 256, в пределах страницы */

/* Memory-mapped (XIP). */
bool QSPIFlash_MemoryMap(const uint8_t **base);
void QSPIFlash_Unmap(void);
bool QSPIFlash_IsMapped(void);

#ifdef __cplusplus
}
#endif

#endif /* QSPI_FLASH_H */

/**
 * @file    sf2_loader.c
 * @brief   Склейка ядра загрузчика (sf2_loader_core) с FatFs/SD и драйвером QSPI Flash.
 */
#include "sf2_loader.h"
#include "qspi_flash.h"

#include "main.h"
#include "ff.h"
#include "sd_storage.h"
#include "display.h"

#include <stdio.h>
#include <string.h>

/* Буфер обмена SD -> QSPI. AXI SRAM: IDMA SDMMC1 читает прямо в него (без bounce-копии). Кратен 256. */
#ifndef SF2_XFER_BYTES
#define SF2_XFER_BYTES  8192U
#endif
static uint8_t s_xfer[SF2_XFER_BYTES] __attribute__((section(".dma_buffer"), aligned(32)));

static FIL            s_file;
static bool           s_file_open;
static bool           s_sd_mounted_by_us;
static bool           s_done;
static bool           s_ok;
static bool           s_progress_shown;
static SF2L_Report    s_report;

/* ------------------------------ источник: SD ------------------------------- */

static SF2SrcStatus src_open(void *ctx, uint32_t *size, uint32_t *stamp)
{
    (void)ctx;
    
    // сигнал датчика на плате игнорируем
    /* if (!SDStorage_CardPresent()) {
        return SF2SRC_NO_MEDIA;
    } */
    if (!SDStorage_IsMounted()) {
        const SDStorage_Result r = SDStorage_Mount();
        if (r != SDS_OK) {
            printf("[SF2] SD card not usable: %s\r\n", SDStorage_ResultName(r));
            SDStorage_PrintDiag();
            return SF2SRC_NO_MEDIA;
        }
        s_sd_mounted_by_us = true;
    }

    FILINFO fno;
    FRESULT fr = f_stat(SF2_SD_PATH, &fno);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        /* Карта есть, а файла нет: чаще всего неверное имя (Windows скрыл расширение: SNDFNT.SF2.sf2). */
        SDStorage_ListRoot();
        return SF2SRC_NO_FILE;
    }
    if (fr != FR_OK) {
        printf("[SF2] f_stat failed: FRESULT=%d\r\n", (int)fr);
        return SF2SRC_ERROR;
    }
    fr = f_open(&s_file, SF2_SD_PATH, FA_READ);
    if (fr != FR_OK) {
        printf("[SF2] f_open failed: FRESULT=%d\r\n", (int)fr);
        return SF2SRC_ERROR;
    }
    s_file_open = true;

    *size  = ((uint64_t)fno.fsize > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (uint32_t)fno.fsize;
    *stamp = ((uint32_t)fno.fdate << 16) | (uint32_t)fno.ftime;
    return SF2SRC_OK;
}

static bool src_read(void *ctx, void *dst, uint32_t len, uint32_t *got)
{
    (void)ctx;
    UINT br = 0;

    if (!s_file_open || f_read(&s_file, dst, (UINT)len, &br) != FR_OK) {
        return false;
    }
    *got = (uint32_t)br;
    return true;
}

static bool src_rewind(void *ctx)
{
    (void)ctx;
    return s_file_open && f_lseek(&s_file, 0) == FR_OK;
}

static void src_close(void *ctx)
{
    (void)ctx;
    if (s_file_open) {
        f_close(&s_file);
        s_file_open = false;
    }
}

static const SF2SourceOps s_src_ops = { src_open, src_read, src_rewind, src_close };

/* ------------------------------ флеш: QSPI --------------------------------- */

static bool op_read(void *c, uint32_t a, void *d, uint32_t n)          { (void)c; return QSPIFlash_Read(a, d, n); }
static bool op_erase_sector(void *c, uint32_t a)                       { (void)c; return QSPIFlash_EraseSector(a); }
static bool op_erase_block(void *c, uint32_t a)                        { (void)c; return QSPIFlash_EraseBlock64(a); }
static bool op_program(void *c, uint32_t a, const void *s, uint32_t n) { (void)c; return QSPIFlash_ProgramPage(a, s, n); }
static bool op_map(void *c, const uint8_t **b)                         { (void)c; return QSPIFlash_MemoryMap(b); }
static void op_unmap(void *c)                                          { (void)c; QSPIFlash_Unmap(); }

static const SF2FlashOps s_flash_ops = {
    QSPIFLASH_SIZE, op_read, op_erase_sector, op_erase_block, op_program, op_map, op_unmap
};

/* ------------------------------ прогресс ----------------------------------- */

static void on_progress(void *ctx, SF2L_Phase phase, uint32_t done, uint32_t total)
{
    static const char *const names[] = { "SF2 check", "SF2 erase", "SF2 write", "SF2 verify" };
    static SF2L_Phase last_phase = (SF2L_Phase)0xFF;
    static int        last_pct   = -1;
    static uint32_t   last_tick  = 0;
    static int        last_print = -1;
    (void)ctx;

    const int pct = total ? (int)(((uint64_t)done * 100U) / total) : 100;
    const uint32_t now = HAL_GetTick();

    if (phase != last_phase) {
        last_phase = phase;
        last_pct = -1;
        last_print = -1;
        printf("[SF2] %s...\r\n", names[phase]);
    }
    if (pct / 10 != last_print) {
        last_print = pct / 10;
        printf("[SF2] %s %d%%\r\n", names[phase], pct);
    }
    /* Дисплей: не чаще раза в 100 мс и только при изменении процента (SPI-вывод медленный). */
    if (pct != last_pct && (pct == 100 || (now - last_tick) >= 100U)) {
        char line[24];
        snprintf(line, sizeof(line), "%s %d%%", names[phase], pct);
        Display_ShowBootProgress(line, (uint8_t)pct);
        s_progress_shown = true;
        last_pct = pct;
        last_tick = now;
    }
}

/* ------------------------------ API ---------------------------------------- */

bool SF2Loader_Ensure(void)
{
    if (s_done) {
        return s_ok;
    }
    s_done = true;

    printf("[SF2] QSPI XIP loader: init QSPI flash\r\n");
    if (!QSPIFlash_Init()) {
        printf("[SF2] QSPI flash init FAILED\r\n");
        return false;
    }

    SF2L_Env env;
    memset(&env, 0, sizeof(env));
    env.flash        = &s_flash_ops;
    env.src          = &s_src_ops;
    env.buf          = s_xfer;
    env.buf_size     = sizeof(s_xfer);
    env.progress     = on_progress;

    const uint32_t t0 = HAL_GetTick();
    const SF2L_Result r = SF2L_Run(&env, &s_report);
    const uint32_t dt = HAL_GetTick() - t0;

    if (s_sd_mounted_by_us) {
        SDStorage_Unmount();        /* SD после обновления не нужна: синтезатор читает только XIP */
        s_sd_mounted_by_us = false;
    }
    if (s_progress_shown) {
        Display_EndBootProgress();
        s_progress_shown = false;
    }

    printf("[SF2] SD file: %s\r\n", SF2L_FileStatusName(s_report.file_status));
    printf("[SF2] Result: %s (%lu ms)\r\n", SF2L_ResultName(r), (unsigned long)dt);

    s_ok = (r == SF2L_OK_UNCHANGED || r == SF2L_OK_UPDATED);
    if (s_ok) {
        printf("[SF2] XIP bank at %p, %lu bytes, CRC32 %08lX\r\n", (const void *)s_report.base,
               (unsigned long)s_report.size, (unsigned long)s_report.crc32);
    }
    return s_ok;
}

bool SF2Loader_GetBank(const uint8_t **base, uint32_t *size)
{
    if (!s_ok || s_report.base == NULL) {
        return false;
    }
    if (base) *base = s_report.base;
    if (size) *size = s_report.size;
    return true;
}

const SF2L_Report *SF2Loader_GetReport(void)
{
    return &s_report;
}

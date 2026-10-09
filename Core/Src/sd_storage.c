/**
 * @file    sd_storage.c
 * @brief   FatFs-драйвер диска поверх HAL SD (SDMMC1, IDMA) с bounce-буфером.
 *
 * Требования к проекту (см. README, раздел "Работа с SD-картой"):
 *  - в CubeMX включён SDMMC1 (4 бита), FATFS в режиме "User-defined";
 *  - вызов MX_SDMMC1_SD_Init() и MX_FATFS_Init() отключён
 *    (Project Manager -> Advanced Settings -> Generated Function Calls);
 *  - FatFs: FF_FS_READONLY = 0 (нужны write и ioctl), FF_MIN_SS = FF_MAX_SS = 512,
 *    FF_LBA64 = 0.
 */
#include "sd_storage.h"

#include "main.h"
#include "sdmmc.h"        /* extern SD_HandleTypeDef hsd1 */
#include "ff.h"
#include "ff_gen_drv.h"

#include <stdint.h>
#include <string.h>

/* ----------------------------- настройки ----------------------------------- */

/* SDMMC_CK = kernel_clk / (2 * SD_CLOCK_DIV). При kernel 192 МГц: 4 -> 24 МГц. */
#ifndef SD_CLOCK_DIV
#define SD_CLOCK_DIV            4U
#endif

/* Размер bounce-буфера в секторах (16 -> 8 КБ в AXI SRAM). */
#ifndef SD_BOUNCE_SECTORS
#define SD_BOUNCE_SECTORS       16U
#endif

/* Максимум секторов за одну прямую DMA-передачу (128 -> 64 КБ). */
#ifndef SD_DIRECT_MAX_SECTORS
#define SD_DIRECT_MAX_SECTORS   128U
#endif

#ifndef SD_READ_TIMEOUT_MS
#define SD_READ_TIMEOUT_MS      1000U
#endif
#ifndef SD_WRITE_TIMEOUT_MS
#define SD_WRITE_TIMEOUT_MS     3000U
#endif

/* Уровень сигнала SD_DETECT, когда карта вставлена (проверьте по схеме платы). */
#ifndef SD_DETECT_ACTIVE_LEVEL
#define SD_DETECT_ACTIVE_LEVEL  GPIO_PIN_RESET
#endif

#define SD_SECTOR_SIZE          512U

/* AXI SRAM: единственная область, про которую точно известно, что IDMA SDMMC1 её видит. */
#define DMA_RAM_BASE            0x24000000UL
#define DMA_RAM_SIZE            (512UL * 1024UL)

/* ------------------------------ состояние ---------------------------------- */

static uint8_t s_bounce[SD_BOUNCE_SECTORS * SD_SECTOR_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32)));

static FATFS s_fs;
static char  s_path[4];                 /* заполняет FATFS_LinkDriver: "0:/" */
static volatile uint8_t s_xfer_state;   /* 0 - идёт, 1 - готово, 2 - ошибка */
static uint8_t s_hw_ready;              /* SDMMC1 + карта инициализированы */
static uint8_t s_linked;
static uint8_t s_mounted;

/* ----------------------- callbacks HAL (вызываются из IRQ) ------------------ */

void HAL_SD_RxCpltCallback(SD_HandleTypeDef *hsd)    { if (hsd == &hsd1) s_xfer_state = 1; }
void HAL_SD_TxCpltCallback(SD_HandleTypeDef *hsd)    { if (hsd == &hsd1) s_xfer_state = 1; }
void HAL_SD_ErrorCallback(SD_HandleTypeDef *hsd)     { if (hsd == &hsd1) s_xfer_state = 2; }
void HAL_SD_AbortCallback(SD_HandleTypeDef *hsd)     { if (hsd == &hsd1) s_xfer_state = 2; }

/* ------------------------------ вспомогательное ----------------------------- */

static inline bool dcache_enabled(void)
{
    return (SCB->CCR & SCB_CCR_DC_Msk) != 0U;
}

/* Буфер можно отдавать IDMA напрямую: он в AXI SRAM и выровнен.
 * 4 байта нужно самому IDMA; при включённом D-cache - 32 байта (строка кэша). */
static bool buffer_is_dma_safe(const void *p, uint32_t bytes)
{
    uint32_t a = (uint32_t)(uintptr_t)p;
    uint32_t align_mask = dcache_enabled() ? 31U : 3U;

    if ((a & align_mask) != 0U) return false;
    if (a < DMA_RAM_BASE || (a + bytes) > (DMA_RAM_BASE + DMA_RAM_SIZE)) return false;
    return true;
}

static int wait_card_ready(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) {
        if ((HAL_GetTick() - t0) > timeout_ms) return -1;
    }
    return 0;
}

static int wait_xfer_done(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (s_xfer_state == 0U) {
        if ((HAL_GetTick() - t0) > timeout_ms) {
            (void)HAL_SD_Abort(&hsd1);
            return -1;
        }
    }
    return (s_xfer_state == 1U) ? 0 : -1;
}

/* Одна DMA-передача n секторов; buf обязан быть DMA-safe. */
static int xfer_read(uint8_t *buf, uint32_t sector, uint32_t n)
{
    if (wait_card_ready(SD_READ_TIMEOUT_MS) != 0) return -1;
    s_xfer_state = 0;
    if (HAL_SD_ReadBlocks_DMA(&hsd1, buf, sector, n) != HAL_OK) return -1;
    if (wait_xfer_done(SD_READ_TIMEOUT_MS) != 0) return -1;
    if (dcache_enabled()) {
        SCB_InvalidateDCache_by_Addr((uint32_t *)buf, (int32_t)(n * SD_SECTOR_SIZE));
    }
    return 0;
}

static int xfer_write(const uint8_t *buf, uint32_t sector, uint32_t n)
{
    if (wait_card_ready(SD_WRITE_TIMEOUT_MS) != 0) return -1;
    if (dcache_enabled()) {
        SCB_CleanDCache_by_Addr((uint32_t *)(uintptr_t)buf, (int32_t)(n * SD_SECTOR_SIZE));
    }
    s_xfer_state = 0;
    if (HAL_SD_WriteBlocks_DMA(&hsd1, (uint8_t *)(uintptr_t)buf, sector, n) != HAL_OK) return -1;
    if (wait_xfer_done(SD_WRITE_TIMEOUT_MS) != 0) return -1;
    /* Карта ещё программирует блоки: дождаться возврата в состояние TRANSFER. */
    return wait_card_ready(SD_WRITE_TIMEOUT_MS);
}

/* ------------------------------ диск-драйвер FatFs -------------------------- */

static DSTATUS sd_status(BYTE lun)
{
    (void)lun;
    return s_hw_ready ? 0 : STA_NOINIT;
}

static DSTATUS sd_initialize(BYTE lun)
{
    /* Само железо инициализирует SDStorage_Mount(); тут только отдаём статус. */
    return sd_status(lun);
}

static DRESULT sd_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
    (void)lun;
    if (!s_hw_ready) return RES_NOTRDY;
    if (count == 0U) return RES_PARERR;

    while (count > 0U) {
        uint32_t n = (count > SD_DIRECT_MAX_SECTORS) ? SD_DIRECT_MAX_SECTORS : count;

        if (buffer_is_dma_safe(buff, n * SD_SECTOR_SIZE)) {
            if (xfer_read(buff, sector, n) != 0) return RES_ERROR;
        } else {
            n = (count > SD_BOUNCE_SECTORS) ? SD_BOUNCE_SECTORS : count;
            if (xfer_read(s_bounce, sector, n) != 0) return RES_ERROR;
            memcpy(buff, s_bounce, n * SD_SECTOR_SIZE);
        }
        buff   += n * SD_SECTOR_SIZE;
        sector += n;
        count  -= n;
    }
    return RES_OK;
}

static DRESULT sd_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
    (void)lun;
    if (!s_hw_ready) return RES_NOTRDY;
    if (count == 0U) return RES_PARERR;

    while (count > 0U) {
        uint32_t n = (count > SD_DIRECT_MAX_SECTORS) ? SD_DIRECT_MAX_SECTORS : count;
        if (buffer_is_dma_safe(buff, n * SD_SECTOR_SIZE)) {
            if (xfer_write(buff, sector, n) != 0) return RES_ERROR;
        } else {
            n = (count > SD_BOUNCE_SECTORS) ? SD_BOUNCE_SECTORS : count;
            memcpy(s_bounce, buff, n * SD_SECTOR_SIZE);
            if (xfer_write(s_bounce, sector, n) != 0) return RES_ERROR;
        }
        buff   += n * SD_SECTOR_SIZE;
        sector += n;
        count  -= n;
    }
    return RES_OK;
}

static DRESULT sd_ioctl(BYTE lun, BYTE cmd, void *buff)
{
    HAL_SD_CardInfoTypeDef info;
    (void)lun;
    if (!s_hw_ready) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        return (wait_card_ready(SD_WRITE_TIMEOUT_MS) == 0) ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        if (HAL_SD_GetCardInfo(&hsd1, &info) != HAL_OK) return RES_ERROR;
        *(DWORD *)buff = info.LogBlockNbr;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (HAL_SD_GetCardInfo(&hsd1, &info) != HAL_OK) return RES_ERROR;
        *(WORD *)buff = (WORD)info.LogBlockSize;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (HAL_SD_GetCardInfo(&hsd1, &info) != HAL_OK) return RES_ERROR;
        *(DWORD *)buff = info.LogBlockSize / SD_SECTOR_SIZE;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static const Diskio_drvTypeDef s_driver = {
    sd_initialize,
    sd_status,
    sd_read,
    sd_write,
    sd_ioctl,
};

/* ------------------------------ публичный API ------------------------------- */

bool SDStorage_CardPresent(void)
{
#ifdef SD_DETECT_Pin
    return HAL_GPIO_ReadPin(SD_DETECT_GPIO_Port, SD_DETECT_Pin) == SD_DETECT_ACTIVE_LEVEL;
#else
    return true;
#endif
}

static int sd_hw_init(void)
{
    /* HAL_SD_DeInit() на необнулённом дескрипторе с Instance == NULL приведёт к HardFault. */
    if (hsd1.State != HAL_SD_STATE_RESET) {
        (void)HAL_SD_DeInit(&hsd1);
    }

    /* Параметры задаём здесь, а не в MX_SDMMC1_SD_Init(): та вызывает Error_Handler()
     * (бесконечный цикл с отключёнными прерываниями), если карты нет. */
    hsd1.Instance                 = SDMMC1;
    hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    hsd1.Init.BusWide             = SDMMC_BUS_WIDE_1B;
    hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd1.Init.ClockDiv            = SD_CLOCK_DIV;

    if (HAL_SD_Init(&hsd1) != HAL_OK) return -1;
    if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK) {
        (void)HAL_SD_DeInit(&hsd1);
        return -1;
    }
    return 0;
}

SDStorage_Result SDStorage_Mount(void)
{
    if (s_mounted) return SDS_OK;

    if (!SDStorage_CardPresent()) return SDS_ERR_NO_CARD;

    if (sd_hw_init() != 0) return SDS_ERR_INIT;
    s_hw_ready = 1;

    if (!s_linked) {
        if (FATFS_LinkDriver(&s_driver, s_path) != 0U) {
            s_hw_ready = 0;
            (void)HAL_SD_DeInit(&hsd1);
            return SDS_ERR_LINK;
        }
        s_linked = 1;
    }

    if (f_mount(&s_fs, s_path, 1) != FR_OK) {
        s_hw_ready = 0;
        (void)HAL_SD_DeInit(&hsd1);
        return SDS_ERR_MOUNT;
    }

    s_mounted = 1;
    return SDS_OK;
}

void SDStorage_Unmount(void)
{
    if (s_mounted) {
        (void)f_mount(NULL, s_path, 0);
        s_mounted = 0;
    }
    if (s_hw_ready) {
        s_hw_ready = 0;
        (void)HAL_SD_DeInit(&hsd1);
    }
}

bool SDStorage_IsMounted(void)
{
    return s_mounted != 0U;
}

const char *SDStorage_Path(void)
{
    return s_path;
}

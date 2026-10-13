/**
 * @file    qspi_flash.c
 * @brief   Драйвер QSPI Flash Winbond W25Q64 на HAL_QSPI (indirect + memory-mapped). См. qspi_flash.h.
 */
#include "qspi_flash.h"
#include "xip_mpu.h"

#include "main.h"
#include "quadspi.h"          /* extern QSPI_HandleTypeDef hqspi; void MX_QUADSPI_Init(void); (генерирует CubeMX) */

#include <stdio.h>
#include <string.h>

/* Коды команд Winbond W25Qxx */
#define CMD_WRITE_ENABLE     0x06U
#define CMD_READ_SR1         0x05U
#define CMD_READ_SR2         0x35U
#define CMD_WRITE_SR2        0x31U
#define CMD_SECTOR_ERASE_4K  0x20U
#define CMD_BLOCK_ERASE_64K  0xD8U
#define CMD_PAGE_PROGRAM     0x02U
#define CMD_FAST_READ_QUAD   0xEBU
#define CMD_JEDEC_ID         0x9FU
#define CMD_RESET_ENABLE     0x66U
#define CMD_RESET            0x99U

#define SR1_BUSY             0x01U
#define SR1_WEL              0x02U
#define SR2_QE               0x02U

#define TIMEOUT_CMD_MS       100U
#define TIMEOUT_PAGE_MS      100U      /* tPP max 3 мс */
#define TIMEOUT_SECTOR_MS    1000U     /* t_SE max 400 мс */
#define TIMEOUT_BLOCK_MS     3000U     /* t_BE64 max 2 с */
#define TIMEOUT_SR_MS        100U

#define READ_CHUNK           0x10000U

static bool    s_inited;
static bool    s_mapped;
static uint8_t s_jedec[3];

/* ------------------------------ команды ------------------------------------ */

static void cmd_init(QSPI_CommandTypeDef *c, uint8_t instruction)
{
    memset(c, 0, sizeof(*c));
    c->Instruction       = instruction;
    c->InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    c->AddressMode       = QSPI_ADDRESS_NONE;
    c->AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    c->DataMode          = QSPI_DATA_NONE;
    c->DdrMode           = QSPI_DDR_MODE_DISABLE;
    c->DdrHoldHalfCycle  = QSPI_DDR_HHC_ANALOG_DELAY;
    c->SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;
}

/* Быстрое чтение 0xEB: инструкция - 1 линия, адрес/режимный байт/данные - 4 линии.
 * Режимный байт 0x00: старшие 4 бита не равны 0xA, значит "continuous read mode" не включается
 * и каждая последующая команда (в том числе стирание) работает как обычно. */
static void cmd_quad_read(QSPI_CommandTypeDef *c, uint32_t addr, uint32_t nbytes)
{
    cmd_init(c, CMD_FAST_READ_QUAD);
    c->AddressMode        = QSPI_ADDRESS_4_LINES;
    c->AddressSize        = QSPI_ADDRESS_24_BITS;
    c->Address            = addr;
    c->AlternateByteMode  = QSPI_ALTERNATE_BYTES_4_LINES;
    c->AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
    c->AlternateBytes     = 0x00U;
    c->DummyCycles        = QSPIFLASH_READ_DUMMY_CYCLES;
    c->DataMode           = QSPI_DATA_4_LINES;
    c->NbData             = nbytes;
}

static bool fail(void)
{
    (void)HAL_QSPI_Abort(&hqspi);   /* вернуть контроллер в READY */
    return false;
}

static bool simple_cmd(uint8_t instruction)
{
    QSPI_CommandTypeDef c;
    cmd_init(&c, instruction);
    return HAL_QSPI_Command(&hqspi, &c, TIMEOUT_CMD_MS) == HAL_OK;
}

static bool read_reg(uint8_t instruction, uint8_t *value)
{
    QSPI_CommandTypeDef c;
    cmd_init(&c, instruction);
    c.DataMode = QSPI_DATA_1_LINE;
    c.NbData   = 1;
    if (HAL_QSPI_Command(&hqspi, &c, TIMEOUT_CMD_MS) != HAL_OK) return false;
    return HAL_QSPI_Receive(&hqspi, value, TIMEOUT_SR_MS) == HAL_OK;
}

/* Аппаратный опрос SR1: ждём (SR1 & mask) == match. */
static bool poll_sr1(uint8_t mask, uint8_t match, uint32_t timeout_ms)
{
    QSPI_CommandTypeDef c;
    QSPI_AutoPollingTypeDef p;

    cmd_init(&c, CMD_READ_SR1);
    c.DataMode = QSPI_DATA_1_LINE;
    c.NbData   = 1;

    memset(&p, 0, sizeof(p));
    p.Match           = match;
    p.Mask            = mask;
    p.MatchMode       = QSPI_MATCH_MODE_AND;
    p.StatusBytesSize = 1;
    p.Interval        = 0x10;
    p.AutomaticStop   = QSPI_AUTOMATIC_STOP_ENABLE;
    return HAL_QSPI_AutoPolling(&hqspi, &c, &p, timeout_ms) == HAL_OK;
}

static bool wait_ready(uint32_t timeout_ms)
{
    return poll_sr1(SR1_BUSY, 0x00U, timeout_ms);
}

static bool write_enable(void)
{
    if (!simple_cmd(CMD_WRITE_ENABLE)) return false;
    return poll_sr1(SR1_WEL, SR1_WEL, TIMEOUT_SR_MS);
}

static bool ensure_indirect(void)
{
    if (s_mapped) {
        QSPIFlash_Unmap();
    }
    return s_inited;
}

/* ------------------------------ публичный API ------------------------------ */

bool QSPIFlash_Init(void)
{
    if (s_inited) return true;

    if (hqspi.State == HAL_QSPI_STATE_RESET) {
        MX_QUADSPI_Init();
    }
    (void)HAL_QSPI_Abort(&hqspi);

    /* Типичные ошибки настройки CubeMX: размер флеша (FSIZE = log2(байт) - 1, для 8 МБ это 22). */
    if (hqspi.Init.FlashSize != 22U) {
        printf("[QSPI] WARNING: hqspi.Init.FlashSize=%lu, expected 22 for 8 MB (see docs/SOUNDFONT_QSPI_SYNTH.md)\r\n",
               (unsigned long)hqspi.Init.FlashSize);
    }

    /* Программный сброс флеша: убирает любое "полузавершённое" состояние после перезагрузки MCU. */
    if (!simple_cmd(CMD_RESET_ENABLE) || !simple_cmd(CMD_RESET)) {
        printf("[QSPI] reset command failed\r\n");
        return false;
    }
    HAL_Delay(2);   /* tRST ~30 мкс, с запасом */

    QSPI_CommandTypeDef c;
    cmd_init(&c, CMD_JEDEC_ID);
    c.DataMode = QSPI_DATA_1_LINE;
    c.NbData   = 3;
    if (HAL_QSPI_Command(&hqspi, &c, TIMEOUT_CMD_MS) != HAL_OK ||
        HAL_QSPI_Receive(&hqspi, s_jedec, TIMEOUT_SR_MS) != HAL_OK) {
        printf("[QSPI] cannot read JEDEC ID (check pins/clock in CubeMX)\r\n");
        return fail();
    }
    printf("[QSPI] JEDEC ID %02X %02X %02X\r\n", s_jedec[0], s_jedec[1], s_jedec[2]);

    /* Третий байт - log2 ёмкости в байтах (0x17 -> 8 МБ). 0x00/0xFF - шина не работает. */
    if (s_jedec[2] < 0x10U || s_jedec[2] > 0x1FU || (1UL << s_jedec[2]) < QSPIFLASH_SIZE) {
        printf("[QSPI] unexpected capacity code 0x%02X (need >= 8 MB)\r\n", s_jedec[2]);
        return false;
    }

    /* Quad Enable: без него IO2/IO3 работают как WP#/HOLD# и 4-проводное чтение невозможно. */
    uint8_t sr2 = 0;
    if (!read_reg(CMD_READ_SR2, &sr2)) return fail();
    if ((sr2 & SR2_QE) == 0U) {
        const uint8_t v = (uint8_t)(sr2 | SR2_QE);

        if (!write_enable()) return fail();
        cmd_init(&c, CMD_WRITE_SR2);
        c.DataMode = QSPI_DATA_1_LINE;
        c.NbData   = 1;
        if (HAL_QSPI_Command(&hqspi, &c, TIMEOUT_CMD_MS) != HAL_OK ||
            HAL_QSPI_Transmit(&hqspi, (uint8_t *)&v, TIMEOUT_SR_MS) != HAL_OK) {
            return fail();
        }
        if (!wait_ready(TIMEOUT_SECTOR_MS)) return fail();
        if (!read_reg(CMD_READ_SR2, &sr2) || (sr2 & SR2_QE) == 0U) {
            printf("[QSPI] cannot set Quad Enable (SR2=0x%02X)\r\n", sr2);
            return false;
        }
        printf("[QSPI] Quad Enable bit set\r\n");
    }

    s_inited = true;
    return true;
}

void QSPIFlash_GetJedecId(uint8_t id[3])
{
    memcpy(id, s_jedec, 3);
}

bool QSPIFlash_Read(uint32_t addr, void *dst, uint32_t len)
{
    uint8_t *p = (uint8_t *)dst;

    if (!ensure_indirect() || (uint64_t)addr + len > QSPIFLASH_SIZE) return false;
    while (len) {
        const uint32_t n = (len > READ_CHUNK) ? READ_CHUNK : len;
        QSPI_CommandTypeDef c;

        cmd_quad_read(&c, addr, n);
        if (HAL_QSPI_Command(&hqspi, &c, TIMEOUT_CMD_MS) != HAL_OK) return fail();
        if (HAL_QSPI_Receive(&hqspi, p, TIMEOUT_SECTOR_MS) != HAL_OK) return fail();
        addr += n;
        p += n;
        len -= n;
    }
    return true;
}

static bool erase(uint8_t instruction, uint32_t addr, uint32_t unit, uint32_t timeout_ms)
{
    if (!ensure_indirect() || (addr % unit) != 0U || addr + unit > QSPIFLASH_SIZE) return false;
    if (!write_enable()) return fail();

    QSPI_CommandTypeDef c;
    cmd_init(&c, instruction);
    c.AddressMode = QSPI_ADDRESS_1_LINE;
    c.AddressSize = QSPI_ADDRESS_24_BITS;
    c.Address     = addr;
    if (HAL_QSPI_Command(&hqspi, &c, TIMEOUT_CMD_MS) != HAL_OK) return fail();
    return wait_ready(timeout_ms) ? true : fail();
}

bool QSPIFlash_EraseSector(uint32_t addr)  { return erase(CMD_SECTOR_ERASE_4K, addr, 0x1000U, TIMEOUT_SECTOR_MS); }
bool QSPIFlash_EraseBlock64(uint32_t addr) { return erase(CMD_BLOCK_ERASE_64K, addr, 0x10000U, TIMEOUT_BLOCK_MS); }

bool QSPIFlash_ProgramPage(uint32_t addr, const void *src, uint32_t len)
{
    if (!ensure_indirect() || len == 0U || len > 256U || ((addr & 0xFFU) + len) > 256U ||
        (uint64_t)addr + len > QSPIFLASH_SIZE) {
        return false;
    }
    if (!write_enable()) return fail();

    QSPI_CommandTypeDef c;
    cmd_init(&c, CMD_PAGE_PROGRAM);
    c.AddressMode = QSPI_ADDRESS_1_LINE;
    c.AddressSize = QSPI_ADDRESS_24_BITS;
    c.Address     = addr;
    c.DataMode    = QSPI_DATA_1_LINE;
    c.NbData      = len;
    if (HAL_QSPI_Command(&hqspi, &c, TIMEOUT_CMD_MS) != HAL_OK) return fail();
    if (HAL_QSPI_Transmit(&hqspi, (uint8_t *)src, TIMEOUT_PAGE_MS) != HAL_OK) return fail();
    return wait_ready(TIMEOUT_PAGE_MS) ? true : fail();
}

bool QSPIFlash_MemoryMap(const uint8_t **base)
{
    if (!s_inited) return false;
    if (s_mapped) {
        if (base) *base = (const uint8_t *)QSPIFLASH_XIP_BASE;
        return true;
    }

    QSPI_CommandTypeDef c;
    QSPI_MemoryMappedTypeDef m;

    cmd_quad_read(&c, 0, 0);
    memset(&m, 0, sizeof(m));
    m.TimeOutPeriod     = 0;
    m.TimeOutActivation = QSPI_TIMEOUT_COUNTER_DISABLE;
    if (HAL_QSPI_MemoryMapped(&hqspi, &c, &m) != HAL_OK) return fail();

    s_mapped = true;
    XipMpu_SetXipEnabled(true);     /* кэшируемое окно открывается только когда отображение уже работает */
    if (base) *base = (const uint8_t *)QSPIFLASH_XIP_BASE;
    return true;
}

void QSPIFlash_Unmap(void)
{
    if (!s_mapped) return;
    XipMpu_SetXipEnabled(false);
    s_mapped = false;
    (void)HAL_QSPI_Abort(&hqspi);
}

bool QSPIFlash_IsMapped(void)
{
    return s_mapped;
}

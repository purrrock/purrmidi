#include "backup.h"

#include <stdio.h>

#include "main.h"

/* Раскладка блока в резервных регистрах RTC. Слова 0..BKP_COUNT_CRC-1 покрыты CRC32, она лежит в слове BKP_CRC_IDX.
 * Слово 31 - рабочее (проверка записи-чтения), в CRC не входит. Менять порядок можно только вместе с BKP_VERSION. */
#define BKP_MAGIC        0x50524D42UL      /* "PRMB" */
#define BKP_VERSION      1UL

enum {
    W_MAGIC = 0,    /* BKP_MAGIC */
    W_VERSION,      /* BKP_VERSION */
    W_SETTINGS,     /* [31] настройки валидны, [23:16] громкость, [15:0] программа */
    W_BOOTS,        /* число загрузок */
    W_WDG_RESETS,   /* число сбросов по сторожевому таймеру */
    W_FAULTS,       /* число сбоев */
    W_RSR,          /* RCC->RSR последней загрузки */
    W_FAULT_KIND,   /* [31] запись о сбое ещё не показана, [7:0] тип сбоя */
    W_FAULT_PC,
    W_FAULT_LR,
    W_FAULT_CFSR,
    W_FAULT_HFSR,
    W_FAULT_UPTIME,
    W_RESERVED0,
    W_RESERVED1,
    W_RESERVED2,
    BKP_COUNT_CRC,  /* = 16: число слов под CRC */
    W_CRC = BKP_COUNT_CRC
};

#define BKP_SCRATCH_IDX  31U
#define SETTINGS_VALID   0x80000000UL
#define FAULT_PENDING    0x80000000UL

static volatile uint32_t *const bkp = &RTC->BKP0R;   /* BKP0R..BKP31R идут подряд с шагом 4 байта */
static BackupState s_state = BACKUP_UNAVAILABLE;

/* Обновление блока (поля + CRC) не должно прерываться обработчиком сбоя, который тоже пишет в блок. */
static uint32_t irq_save(void)
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void irq_restore(uint32_t primask)
{
    if (primask == 0U) {
        __enable_irq();
    }
}

static uint32_t crc32_words(uint32_t first, uint32_t count)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = first; i < first + count; ++i) {
        const uint32_t w = bkp[i];
        for (uint32_t b = 0; b < 4U; ++b) {
            crc ^= (w >> (8U * b)) & 0xFFU;
            for (uint32_t k = 0; k < 8U; ++k) {
                crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
            }
        }
    }
    return ~crc;
}

static void seal(void)
{
    bkp[W_CRC] = crc32_words(0U, BKP_COUNT_CRC);
}

static int block_valid(void)
{
    return bkp[W_MAGIC] == BKP_MAGIC && bkp[W_VERSION] == BKP_VERSION && bkp[W_CRC] == crc32_words(0U, BKP_COUNT_CRC);
}

static void block_clear(void)
{
    for (uint32_t i = 0; i < BKP_COUNT_CRC; ++i) {
        bkp[i] = 0U;
    }
    bkp[W_MAGIC]   = BKP_MAGIC;
    bkp[W_VERSION] = BKP_VERSION;
    seal();
}

/* Запись и чтение через рабочее слово: доказывает, что доступ к домену открыт и тактирование есть. */
static int selftest(void)
{
    static const uint32_t patterns[2] = { 0xA5C3965AUL, 0x5A3C69A5UL };
    for (uint32_t i = 0; i < 2U; ++i) {
        bkp[BKP_SCRATCH_IDX] = patterns[i];
        if (bkp[BKP_SCRATCH_IDX] != patterns[i]) {
            return 0;
        }
    }
    return 1;
}

static void enable_access(void)
{
    HAL_PWR_EnableBkUpAccess();                 /* PWR_CR1.DBP */
    const uint32_t t0 = HAL_GetTick();
    while ((PWR->CR1 & PWR_CR1_DBP) == 0U && (HAL_GetTick() - t0) < 10U) { }
    __HAL_RCC_RTC_CLK_ENABLE();                 /* тактирование APB-интерфейса RTC (RCC_APB4ENR.RTCAPBEN) */
}

BackupState Backup_Init(void)
{
    enable_access();

    if (!selftest()) {
        /* Запасной вариант: если источник тактирования RTC ещё не выбран (после сброса домена), выбрать LSI (он уже
         * запущен IWDG) и включить RTC. Уже выбранный источник не трогаем: смена потребовала бы сброса домена. */
        if ((RCC->BDCR & RCC_BDCR_RTCSEL) == 0U) {
            RCC->BDCR |= RCC_BDCR_RTCSEL_1 | RCC_BDCR_RTCEN;     /* RTCSEL = 10b: LSI */
        }
        if (!selftest()) {
            s_state = BACKUP_UNAVAILABLE;
            printf("[BKP] backup registers not accessible - settings will not be saved\r\n");
            return s_state;
        }
    }

    if (block_valid()) {
        s_state = BACKUP_RESTORED;
        printf("[BKP] saved data found (backup domain kept: VDD or VBAT battery)\r\n");
    } else {
        block_clear();
        s_state = BACKUP_EMPTY;
        printf("[BKP] no saved data (no VBAT battery, first power-up or data corrupted) - using default settings\r\n");
    }
    return s_state;
}

BackupState Backup_GetState(void)
{
    return s_state;
}

void Backup_NoteReset(uint32_t rsr)
{
    if (s_state == BACKUP_UNAVAILABLE) {
        return;
    }
    const uint32_t pm = irq_save();
    bkp[W_BOOTS]++;
    if ((rsr & (RCC_RSR_IWDG1RSTF | RCC_RSR_WWDG1RSTF)) != 0U) {
        bkp[W_WDG_RESETS]++;
    }
    bkp[W_RSR] = rsr;
    seal();
    irq_restore(pm);
}

int Backup_LoadSettings(uint16_t *program, uint8_t *volume)
{
    if (s_state != BACKUP_RESTORED && s_state != BACKUP_EMPTY) {
        return 0;
    }
    const uint32_t w = bkp[W_SETTINGS];
    if ((w & SETTINGS_VALID) == 0U) {
        return 0;
    }
    const uint32_t vol = (w >> 16) & 0xFFU;
    if (vol > 127U) {
        return 0;
    }
    if (program) { *program = (uint16_t)(w & 0xFFFFU); }
    if (volume)  { *volume  = (uint8_t)vol; }
    return 1;
}

void Backup_SaveSettings(uint16_t program, uint8_t volume)
{
    if (s_state == BACKUP_UNAVAILABLE) {
        return;
    }
    if (volume > 127U) {
        volume = 127U;
    }
    const uint32_t w = SETTINGS_VALID | ((uint32_t)volume << 16) | program;
    if (bkp[W_SETTINGS] == w) {
        return;                                 /* без изменений: не пишем зря */
    }
    const uint32_t pm = irq_save();
    bkp[W_SETTINGS] = w;
    seal();
    irq_restore(pm);
}

void Backup_GetStats(BackupStats *out)
{
    if (!out) {
        return;
    }
    out->boot_count = out->wdg_reset_count = out->fault_count = 0U;
    if (s_state == BACKUP_UNAVAILABLE) {
        return;
    }
    out->boot_count      = bkp[W_BOOTS];
    out->wdg_reset_count = bkp[W_WDG_RESETS];
    out->fault_count     = bkp[W_FAULTS];
}

void Backup_RecordFault(uint32_t kind, uint32_t pc, uint32_t lr, uint32_t cfsr, uint32_t hfsr)
{
    /* Вызывается и до Backup_Init() (сбой в начале загрузки): доступ открываем сами, это только запись в регистры. */
    if (s_state == BACKUP_UNAVAILABLE) {
        PWR->CR1 |= PWR_CR1_DBP;
        RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;
        (void)RCC->APB4ENR;
        if (!selftest()) {
            return;
        }
        if (!block_valid()) {
            block_clear();
        }
    }
    bkp[W_FAULTS]++;
    bkp[W_FAULT_KIND]   = FAULT_PENDING | (kind & 0xFFU);
    bkp[W_FAULT_PC]     = pc;
    bkp[W_FAULT_LR]     = lr;
    bkp[W_FAULT_CFSR]   = cfsr;
    bkp[W_FAULT_HFSR]   = hfsr;
    bkp[W_FAULT_UPTIME] = HAL_GetTick();
    seal();
}

int Backup_TakeFault(BackupFault *out)
{
    if (s_state == BACKUP_UNAVAILABLE) {
        return 0;
    }
    const uint32_t k = bkp[W_FAULT_KIND];
    if ((k & FAULT_PENDING) == 0U) {
        return 0;
    }
    if (out) {
        out->kind      = k & 0xFFU;
        out->pc        = bkp[W_FAULT_PC];
        out->lr        = bkp[W_FAULT_LR];
        out->cfsr      = bkp[W_FAULT_CFSR];
        out->hfsr      = bkp[W_FAULT_HFSR];
        out->uptime_ms = bkp[W_FAULT_UPTIME];
    }
    const uint32_t pm = irq_save();
    bkp[W_FAULT_KIND] = k & ~FAULT_PENDING;
    seal();
    irq_restore(pm);
    return 1;
}

void Backup_ReportPrevious(void)
{
    if (s_state == BACKUP_UNAVAILABLE) {
        return;
    }
    static const char *const kinds[] = { "HardFault", "MemManage", "BusFault", "UsageFault", "Error_Handler" };
    BackupStats st;
    Backup_GetStats(&st);
    printf("[BKP] boots=%lu, watchdog resets=%lu, faults=%lu\r\n",
           (unsigned long)st.boot_count, (unsigned long)st.wdg_reset_count, (unsigned long)st.fault_count);

    BackupFault f;
    if (Backup_TakeFault(&f)) {
        printf("[BKP] previous crash: %s at uptime %lu ms, PC=0x%08lX LR=0x%08lX CFSR=0x%08lX HFSR=0x%08lX\r\n",
               kinds[f.kind <= 4U ? f.kind : 0U], (unsigned long)f.uptime_ms,
               (unsigned long)f.pc, (unsigned long)f.lr, (unsigned long)f.cfsr, (unsigned long)f.hfsr);
        printf("[BKP] decode: arm-none-eabi-addr2line -f -C -e build/Release/purrmidi.elf <PC> <LR>\r\n");
    }
}

/**
 * @file    xip_diag.c
 * @brief   См. xip_diag.h.
 */
#include "xip_diag.h"
#include "qspi_flash.h"

#include "main.h"
#include "quadspi.h"

#include <stdio.h>

#define BLOCK_FRAMES      128U      /* как половина DMA-буфера */
#define BLOCK_BUDGET_US   2666U

static uint32_t cycles_per_us(void)
{
    const uint32_t v = SystemCoreClock / 1000000U;
    return v ? v : 1U;
}

/* ------------------------------ конфигурация ------------------------------- */

static void print_mpu_region(uint32_t n)
{
    MPU->RNR = n;
    const uint32_t rbar = MPU->RBAR;
    const uint32_t rasr = MPU->RASR;

    printf("[XIP]   MPU%lu base=0x%08lX size=2^%lu en=%lu AP=%lu TEX=%lu S=%lu C=%lu B=%lu XN=%lu\r\n",
           (unsigned long)n, (unsigned long)(rbar & 0xFFFFFFE0UL), (unsigned long)(((rasr >> 1) & 0x1FU) + 1U),
           (unsigned long)(rasr & 1U), (unsigned long)((rasr >> 24) & 7U), (unsigned long)((rasr >> 19) & 7U),
           (unsigned long)((rasr >> 18) & 1U), (unsigned long)((rasr >> 17) & 1U), (unsigned long)((rasr >> 16) & 1U),
           (unsigned long)((rasr >> 28) & 1U));
}

void XipDiag_PrintConfig(void)
{
    const uint32_t ccr = SCB->CCR;
    const int dcache = (ccr & SCB_CCR_DC_Msk) != 0U;
    const int icache = (ccr & SCB_CCR_IC_Msk) != 0U;

    printf("[XIP] --- configuration ---\r\n");
    printf("[XIP] D-cache %s, I-cache %s (SCB->CCR=0x%08lX), SystemCoreClock=%lu Hz\r\n",
           dcache ? "ON" : "OFF", icache ? "ON" : "OFF", (unsigned long)ccr, (unsigned long)SystemCoreClock);

    const uint32_t saved_rnr = MPU->RNR;
    printf("[XIP] MPU CTRL=0x%08lX\r\n", (unsigned long)MPU->CTRL);
    for (uint32_t i = 0; i <= 4U; i++) {
        print_mpu_region(i);
    }
    MPU->RNR = 1U;
    const uint32_t xip_rasr = MPU->RASR;
    MPU->RNR = saved_rnr;

    const uint32_t cr  = QUADSPI->CR;
    const uint32_t dcr = QUADSPI->DCR;
    const uint32_t qc  = QUADSPI->CCR;
    const uint32_t hclk = HAL_RCC_GetHCLKFreq();
    const uint32_t sel = (RCC->D1CCIPR & RCC_D1CCIPR_QSPISEL) >> RCC_D1CCIPR_QSPISEL_Pos;
    const uint32_t presc = cr >> 24;

    printf("[XIP] QUADSPI CR=0x%08lX DCR=0x%08lX CCR=0x%08lX\r\n", (unsigned long)cr, (unsigned long)dcr, (unsigned long)qc);
    printf("[XIP]   prescaler=%lu (divide by %lu), sample shift=%lu, FIFO thr=%lu, timeout counter=%lu\r\n",
           (unsigned long)presc, (unsigned long)(presc + 1U), (unsigned long)((cr >> 4) & 1U),
           (unsigned long)((cr >> 8) & 0xFU), (unsigned long)((cr >> 3) & 1U));
    printf("[XIP]   FSIZE=%lu (flash 2^%lu bytes), CS high=%lu cycles, CK mode=%lu\r\n", (unsigned long)((dcr >> 16) & 0x1FU),
           (unsigned long)(((dcr >> 16) & 0x1FU) + 1U), (unsigned long)(((dcr >> 8) & 7U) + 1U), (unsigned long)(dcr & 1U));
    printf("[XIP]   read cmd: FMODE=%lu (3=memory-mapped) INSTR=0x%02lX IMODE=%lu ADMODE=%lu ADSIZE=%lu ABMODE=%lu DCYC=%lu DMODE=%lu SIOO=%lu DDR=%lu\r\n",
           (unsigned long)((qc >> 26) & 3U), (unsigned long)(qc & 0xFFU), (unsigned long)((qc >> 8) & 3U),
           (unsigned long)((qc >> 10) & 3U), (unsigned long)((qc >> 12) & 3U), (unsigned long)((qc >> 14) & 3U),
           (unsigned long)((qc >> 18) & 0x1FU), (unsigned long)((qc >> 24) & 3U), (unsigned long)((qc >> 28) & 1U),
           (unsigned long)((qc >> 31) & 1U));
    if (sel == 0U) {
        printf("[XIP]   QSPI kernel clock = HCLK %lu Hz -> QSPI clock %lu.%lu MHz\r\n", (unsigned long)hclk,
               (unsigned long)(hclk / (presc + 1U) / 1000000U), (unsigned long)((hclk / (presc + 1U) / 100000U) % 10U));
    } else {
        printf("[XIP]   QSPI kernel clock source=%lu (not HCLK), HCLK=%lu Hz\r\n", (unsigned long)sel, (unsigned long)hclk);
    }

    /* Вердикт: типичные причины медленного XIP. */
    int problems = 0;
    if (!dcache)                      { printf("[XIP] PROBLEM: D-cache is OFF\r\n"); problems++; }
    if (!QSPIFlash_IsMapped())        { printf("[XIP] PROBLEM: QSPI is not in memory-mapped mode\r\n"); problems++; }
    if ((xip_rasr & 1U) == 0U)        { printf("[XIP] PROBLEM: MPU region 1 (XIP window) is disabled\r\n"); problems++; }
    else if ((xip_rasr & (1U << 17)) == 0U) { printf("[XIP] PROBLEM: MPU region 1 is not cacheable\r\n"); problems++; }
    if (((qc >> 26) & 3U) != 3U)      { printf("[XIP] PROBLEM: QUADSPI FMODE is not memory-mapped\r\n"); problems++; }
    if (((qc >> 24) & 3U) != 3U || ((qc >> 10) & 3U) != 3U) {
        printf("[XIP] PROBLEM: read command is not 4-line (ADMODE/DMODE)\r\n"); problems++;
    }
    if (((cr >> 3) & 1U) != 0U)       { printf("[XIP] note: timeout counter is enabled\r\n"); }
    if (problems == 0) {
        printf("[XIP] config verdict: OK\r\n");
    }
}

/* ------------------------------ бенчмарк ----------------------------------- */

static void invalidate(const void *p, uint32_t n)
{
    const uint32_t a = (uint32_t)p & ~31UL;
    const uint32_t end = ((uint32_t)p + n + 31UL) & ~31UL;
    SCB_InvalidateDCache_by_Addr((volatile void *)a, (int32_t)(end - a));
}

static void print_x10(const char *name, uint32_t x10, const char *unit)
{
    printf("[XIP]   %-44s %lu.%lu %s\r\n", name, (unsigned long)(x10 / 10U), (unsigned long)(x10 % 10U), unit);
}

/* Чтение подряд: холодный кэш (каждая строка - обращение к флешу), затем тёплый. Цикл сравнивается с таким же
 * циклом по массиву в DTCM: в Debug (-O0) накладные расходы цикла (~20 тактов на чтение) полностью скрывают
 * разницу кэша, поэтому важны значения "net" (за вычетом базового цикла). */
static volatile int16_t s_baseline[512];

static void bench_sequential(const int16_t *a)
{
    enum { N = 4096 };            /* 8 КБ: заведомо помещается в D-cache (16 КБ) */
    volatile const int16_t *p = a;
    int32_t sum = 0;

    uint32_t t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) { sum += s_baseline[i & 511U]; }
    const uint32_t base = DWT->CYCCNT - t0;

    invalidate(a, N * 2U);
    t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) { sum += p[i]; }
    const uint32_t cold = DWT->CYCCNT - t0;

    t0 = DWT->CYCCNT;
    for (uint32_t i = 0; i < N; i++) { sum += p[i]; }
    const uint32_t warm = DWT->CYCCNT - t0;

    print_x10("loop overhead (same loop over DTCM)", (base * 10U) / N, "cycles/sample");
    print_x10("sequential int16, cold cache (total)", (cold * 10U) / N, "cycles/sample");
    print_x10("sequential int16, warm cache (total)", (warm * 10U) / N, "cycles/sample");
    print_x10("  net of loop overhead, cold", (cold > base ? cold - base : 0U) * 10U / N, "cycles/sample");
    print_x10("  net of loop overhead, warm", (warm > base ? warm - base : 0U) * 10U / N, "cycles/sample");
    print_x10("sequential, cold throughput", ((N * 2U) * 10U) / ((cold / cycles_per_us()) ? (cold / cycles_per_us()) : 1U),
              "MB/s");
    (void)sum;
}

/* Задержка одного обращения с промахом: инвалидируем строку и читаем одно значение. */
static void bench_latency(const int16_t *a, uint32_t samples)
{
    volatile const int16_t *p = a;
    uint32_t seed = 12345U, sum = 0, mn = 0xFFFFFFFFU, mx = 0;
    enum { N = 200 };

    for (uint32_t i = 0; i < N; i++) {
        seed = seed * 1664525U + 1013904223U;
        const uint32_t idx = (seed >> 8) % (samples - 64U);
        invalidate((const void *)&p[idx], 2U);
        __DSB();
        const uint32_t t0 = DWT->CYCCNT;
        const int16_t v = p[idx];
        const uint32_t dt = DWT->CYCCNT - t0;
        (void)v;
        sum += dt;
        if (dt < mn) mn = dt;
        if (dt > mx) mx = dt;
    }
    const uint32_t us = cycles_per_us();
    printf("[XIP]   single random read (miss): avg=%lu min=%lu max=%lu cycles (avg %lu ns)\r\n", (unsigned long)(sum / N),
           (unsigned long)mn, (unsigned long)mx, (unsigned long)((sum / N) * 1000U / us));
}

/* Модель голосов TinySoundFont: voice-major, на каждом кадре два чтения (pos, pos+1), шаг pitch (16.16). */
static uint32_t bench_streams(const int16_t *a, uint32_t samples, const char *tag)
{
    uint32_t ref_avg_us = 0;
    static const uint32_t counts[] = { 1, 4, 8, 16, 32 };
    static const uint32_t steps[]  = { 0x10000U, 0x20000U };   /* высота 1.0 и 2.0 */
    enum { BLOCKS = 40 };
    const uint32_t us = cycles_per_us();

    for (unsigned si = 0; si < 2U; si++) {
        for (unsigned ci = 0; ci < 5U; ci++) {
            const uint32_t n = counts[ci];
            uint32_t pos[32];
            volatile const int16_t *p = a;
            int32_t sum = 0;
            uint32_t total = 0, worst = 0;

            invalidate(a, samples * 2U);
            for (uint32_t i = 0; i < n; i++) {
                pos[i] = ((samples / 2U) / 32U * i) << 16;     /* потоки в разных местах банка */
            }
            for (uint32_t b = 0; b < BLOCKS; b++) {
                const uint32_t t0 = DWT->CYCCNT;
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t ps = pos[i];
                    for (uint32_t f = 0; f < BLOCK_FRAMES; f++) {
                        const uint32_t idx = ps >> 16;
                        sum += p[idx] + p[idx + 1U];
                        ps += steps[si];
                    }
                    pos[i] = ps;
                }
                const uint32_t dt = DWT->CYCCNT - t0;
                total += dt;
                if (dt > worst) worst = dt;
            }
            const uint32_t avg_us = total / BLOCKS / us, max_us = worst / us;
            printf("[XIP]   %s pitch %lu.0 voices=%2lu: reads only, avg=%4lu us max=%4lu us per 128-frame block (%lu%% of %lu us)%s\r\n",
                   tag, (unsigned long)(steps[si] >> 16), (unsigned long)n, (unsigned long)avg_us, (unsigned long)max_us,
                   (unsigned long)(max_us * 100U / BLOCK_BUDGET_US), (unsigned long)BLOCK_BUDGET_US,
                   (max_us > BLOCK_BUDGET_US) ? "  <-- OVER BUDGET" : "");
            if (si == 0U && n == 32U) ref_avg_us = avg_us;
            (void)sum;
        }
    }
    return ref_avg_us;
}

void XipDiag_Bench(const uint8_t *bank, uint32_t bank_size)
{
    if (bank == NULL || bank_size < 65536U) {
        printf("[XIP] bench skipped (no bank)\r\n");
        return;
    }
    if (!QSPIFlash_IsMapped()) {
        printf("[XIP] bench skipped (QSPI is not memory-mapped)\r\n");
        return;
    }
    const int16_t *a = (const int16_t *)(((uint32_t)bank + 31U) & ~31UL);
    const uint32_t samples = (bank_size - 64U) / 2U;

    printf("[XIP] --- raw XIP read speed (no synth) ---\r\n");
    bench_sequential(a);
    bench_latency(a, samples);
    const uint32_t on_us = bench_streams(a, samples, "D-cache ON ");

    printf("[XIP] --- same with D-cache OFF ---\r\n");
    SCB_DisableDCache();
    const uint32_t off_us = bench_streams(a, samples, "D-cache OFF");
    SCB_EnableDCache();

    if (on_us > 0U && off_us * 10U < on_us * 12U) {
        printf("[XIP] PROBLEM: D-cache speeds XIP reads up by less than 1.2x (ON %lu us, OFF %lu us at 32 voices)\r\n",
               (unsigned long)on_us, (unsigned long)off_us);
    } else if (on_us > 0U) {
        printf("[XIP] D-cache verdict: OK, XIP reads %lu.%lux faster with D-cache (32 voices: ON %lu us, OFF %lu us)\r\n",
               (unsigned long)(off_us / on_us), (unsigned long)((off_us * 10U / on_us) % 10U), (unsigned long)on_us,
               (unsigned long)off_us);
    }
    printf("[XIP] --- bench done ---\r\n");
}

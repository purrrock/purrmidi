/**
 * @file    xip_mpu.h
 * @brief   MPU и D-cache для XIP-чтения SoundFont из QSPI Flash (0x90000000).
 *
 * Зачем D-cache. Без него каждое чтение int16 из XIP - отдельная QSPI-транзакция (сотни нс), а TinySoundFont
 * на каждый отсчёт каждого голоса читает два сэмпла. С D-cache сэмплы подтягиваются строками по 32 байта
 * (16 отсчётов), и пока голос читает последовательно, на 16 чтений приходится одно обращение к флешу.
 *
 * Чтобы включить D-cache безопасно (не ломая DMA и SDMMC IDMA), DMA-буферы лежат только в D2 SRAM (.dma_buffer),
 * которая объявлена Normal non-cacheable. AXI SRAM (арена голосов и куча) кэшируется write-back: на ней нет DMA.
 * Кэшируется также XIP-окно (write-through, только чтение).
 *
 * Регионы MPU (регион 0 из main.c MPU_Config() - "всё недоступно, кроме памяти по умолчанию" - не трогается;
 * для 0x90000000 он служит защитой от спекулятивных обращений за пределы 8 МБ):
 *   1: 0x90000000, 8 МБ, RO, cacheable write-through. ВКЛЮЧАЕТСЯ ТОЛЬКО пока QSPI в Memory-Mapped режиме
 *      (иначе спекулятивное чтение Cortex-M7 в окно без отображения может подвесить шину);
 *   2: AXI SRAM  0x24000000, 512 КБ  - Normal, write-back + write-allocate (только данные CPU: .synth_arena, .axi_heap);
 *   3: SRAM1-3   0x30000000, 512 КБ  - non-cacheable (физически 288 КБ): все DMA-буферы (.dma_buffer);
 *   4: SRAM4     0x38000000,  64 КБ  - non-cacheable.
 */
#ifndef XIP_MPU_H
#define XIP_MPU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Настраивает регионы 1..4 и включает MPU. Вызывать после MPU_Config(), ДО SCB_EnableDCache(). */
void XipMpu_Config(void);

/** Включает/выключает кэшируемое XIP-окно (вызывается драйвером QSPI при входе/выходе из Memory-Mapped). */
void XipMpu_SetXipEnabled(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* XIP_MPU_H */

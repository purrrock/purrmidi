/**
 * @file    xip_mpu.c
 * @brief   См. xip_mpu.h.
 */
#include "xip_mpu.h"
#include "qspi_flash.h"

#include "stm32h7xx_hal.h"

#define XIP_REGION   MPU_REGION_NUMBER1

static void set_region(uint8_t number, uint32_t base, uint8_t size, uint8_t access, uint8_t tex,
                       uint8_t cacheable, uint8_t bufferable, uint8_t xn, uint8_t enable)
{
    MPU_Region_InitTypeDef r = {0};

    r.Enable           = enable;
    r.Number           = number;
    r.BaseAddress      = base;
    r.Size             = size;
    r.SubRegionDisable = 0x00;
    r.TypeExtField     = tex;
    r.AccessPermission = access;
    r.DisableExec      = xn;
    r.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    r.IsCacheable      = cacheable;
    r.IsBufferable     = bufferable;   /* C=1,B=0 -> write-through; C=1,B=1 (TEX=1) -> write-back + write-allocate; C=0,B=0 -> non-cacheable */
    HAL_MPU_ConfigRegion(&r);
}

void XipMpu_Config(void)
{
    HAL_MPU_Disable();

    /* XIP: read-only, write-through cacheable, не исполняемая; выключена до входа в Memory-Mapped. */
    set_region(XIP_REGION, QSPIFLASH_XIP_BASE, MPU_REGION_SIZE_8MB, MPU_REGION_PRIV_RO_URO,
               MPU_TEX_LEVEL0, MPU_ACCESS_CACHEABLE, MPU_ACCESS_NOT_BUFFERABLE,
               MPU_INSTRUCTION_ACCESS_DISABLE, MPU_REGION_DISABLE);

    /* AXI SRAM: Normal, WRITE-BACK + write-allocate (TEX=001, C=1, B=1). Здесь только данные, которые читает и пишет
     * CPU: арена голосов движков (.synth_arena) и куча malloc (.axi_heap, метаданные TinySoundFont и т.п.). DMA в эту
     * область НЕ работает (см. D2 ниже), поэтому операции с кэшем не нужны, а доступ к линиям задержки физических
     * моделей идёт через D-cache. Любой новый DMA-буфер обязан лежать в .dma_buffer (D2). */
    set_region(MPU_REGION_NUMBER2, 0x24000000UL, MPU_REGION_SIZE_512KB, MPU_REGION_FULL_ACCESS,
               MPU_TEX_LEVEL1, MPU_ACCESS_CACHEABLE, MPU_ACCESS_BUFFERABLE,
               MPU_INSTRUCTION_ACCESS_ENABLE, MPU_REGION_ENABLE);

    /* D2 SRAM (.dma_buffer: кольцо SAI1, bounce-буфер SD, буфер загрузчика): Normal NON-cacheable (TEX=001, C=0, B=0).
     * DMA и CPU видят одни и те же данные без чистки/инвалидации кэша. */
    set_region(MPU_REGION_NUMBER3, 0x30000000UL, MPU_REGION_SIZE_512KB, MPU_REGION_FULL_ACCESS,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE, MPU_ACCESS_NOT_BUFFERABLE,
               MPU_INSTRUCTION_ACCESS_ENABLE, MPU_REGION_ENABLE);
    set_region(MPU_REGION_NUMBER4, 0x38000000UL, MPU_REGION_SIZE_64KB, MPU_REGION_FULL_ACCESS,
               MPU_TEX_LEVEL1, MPU_ACCESS_NOT_CACHEABLE, MPU_ACCESS_NOT_BUFFERABLE,
               MPU_INSTRUCTION_ACCESS_ENABLE, MPU_REGION_ENABLE);

    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

void XipMpu_SetXipEnabled(bool enable)
{
    __DSB();
    MPU->RNR = XIP_REGION;

    if (enable) {
        MPU->RASR |= MPU_RASR_ENABLE_Msk;
        __DSB();
        __ISB();
        /* Строки, оставшиеся от прошлой сессии (флеш мог быть перезаписан), недействительны. */
        SCB_InvalidateDCache_by_Addr((volatile void *)QSPIFLASH_XIP_BASE, (int32_t)QSPIFLASH_SIZE);
    } else {
        SCB_InvalidateDCache_by_Addr((volatile void *)QSPIFLASH_XIP_BASE, (int32_t)QSPIFLASH_SIZE);
        __DSB();
        MPU->RASR &= ~MPU_RASR_ENABLE_Msk;
        __DSB();
        __ISB();
    }
}

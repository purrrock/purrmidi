/**
 * @file    xip_diag.h
 * @brief   Диагностика XIP-чтения SoundFont из QSPI Flash (UART, при старте).
 *
 * XipDiag_PrintConfig() - что реально включено в железе: D-cache, MPU-регион XIP, регистры QUADSPI,
 *                         тактовая частота QSPI. В конце - вердикт "OK" или "PROBLEM: ...".
 * XipDiag_Bench()       - скорость чтения банка из XIP без синтезатора: холодный/тёплый кэш, задержка
 *                         одного обращения и модель голосов (N потоков чтения по 128 кадров на блок),
 *                         с включённым и выключенным D-cache.
 * Вызывать до запуска аудио DMA (в бенчмарке D-cache временно выключается).
 */
#ifndef XIP_DIAG_H
#define XIP_DIAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void XipDiag_PrintConfig(void);
void XipDiag_Bench(const uint8_t *bank, uint32_t bank_size);

#ifdef __cplusplus
}
#endif

#endif /* XIP_DIAG_H */

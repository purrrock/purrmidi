/**
 * @file    pm_crc32.h
 * @brief   CRC-32 (IEEE 802.3 / zlib). Потоковое использование:
 *            uint32_t crc = 0;
 *            crc = PM_Crc32_Update(crc, buf1, n1);
 *            crc = PM_Crc32_Update(crc, buf2, n2);
 *          Контрольный вектор: CRC32("123456789") = 0xCBF43926.
 */
#ifndef PM_CRC32_H
#define PM_CRC32_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t PM_Crc32_Update(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PM_CRC32_H */

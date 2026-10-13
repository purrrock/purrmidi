/**
 * @file    sf2_loader.h
 * @brief   Загрузка SoundFont-банка в QSPI Flash с SD-карты и включение XIP (0x90000000).
 *
 * Вызывается один раз при старте (из SF2Cache_OpenFile(), т.е. из SoundFontSynth_InitSF2()):
 *   1. инициализация QSPI Flash;
 *   2. если есть SD-карта с 0:/SNDFNT.SF2 - проверка (RIFF/sfbk, размер, CRC32 против заголовка во флеше);
 *   3. новый/изменённый файл - стирание и запись, проверка CRC32 через XIP, запись заголовка;
 *   4. QSPI переводится в Memory-Mapped режим; синтезатор читает сэмплы напрямую по указателям.
 * Повторные вызовы возвращают результат первого.
 */
#ifndef SF2_LOADER_H
#define SF2_LOADER_H

#include <stdbool.h>
#include <stdint.h>

#include "sf2_loader_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Путь файла банка на SD-карте. */
#ifndef SF2_SD_PATH
#define SF2_SD_PATH "0:/SNDFNT.SF2"
#endif

/** Выполняет сценарий загрузки один раз. true - банк доступен через XIP. */
bool SF2Loader_Ensure(void);

/** Указатель на начало банка в XIP-окне и его размер. false, если банка нет. */
bool SF2Loader_GetBank(const uint8_t **base, uint32_t *size);

/** Результат последнего выполнения сценария (для диагностики). */
const SF2L_Report *SF2Loader_GetReport(void);

#ifdef __cplusplus
}
#endif

#endif /* SF2_LOADER_H */

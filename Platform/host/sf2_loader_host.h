/**
 * @file    sf2_loader_host.h
 * @brief   Хост-реализация API sf2_loader.h для тестов и Windows-приложения.
 *
 * На прошивке банк SoundFont лежит в QSPI Flash и читается через XIP (Platform/qspi_xip/sf2_loader.c).
 * На ПК роль «флеша» играет область ОЗУ, в которую файл банка читается целиком: синтезатор работает
 * через те же SF2Loader_Ensure()/SF2Loader_GetBank() и читает PCM по указателю, как на плате.
 */
#ifndef SF2_LOADER_HOST_H
#define SF2_LOADER_HOST_H

#include "sf2_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Явно задать файл банка (NULL - вернуться к поиску по умолчанию). Сбрасывает ранее загруженный банк:
 * следующий SF2Loader_Ensure() читает файл заново. Если файл не открывается, обращение к банку
 * завершается ошибкой (поиск по умолчанию в этом случае не выполняется).
 *
 * Поиск по умолчанию: переменная окружения PURRMIDI_TEST_SF2, затем tests/SNDFNT.SF2, SNDFNT.SF2 и
 * SoundFonts/Yamaha S90ES lite.SF2 относительно текущего каталога.
 */
void SF2LoaderHost_SetPath(const char *path);

/** Освободить банк в ОЗУ и сбросить состояние загрузчика. */
void SF2LoaderHost_Unload(void);

#ifdef __cplusplus
}
#endif

#endif /* SF2_LOADER_HOST_H */

/**
 * @file    sf2_loader_web.h
 * @brief   Банк SoundFont в памяти WebAssembly-модуля (замена QSPI XIP / файла на хосте).
 */
#ifndef SF2_LOADER_WEB_H
#define SF2_LOADER_WEB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Выделяет буфер под банк (предыдущий освобождается). nullptr, если не хватило памяти. */
uint8_t *SF2LoaderWeb_Alloc(uint32_t size);

/** Проверяет заголовок RIFF/sfbk и делает банк доступным для SF2Loader_Ensure(). */
bool SF2LoaderWeb_Commit(void);

/** Освобождает банк. */
void SF2LoaderWeb_Unload(void);

#ifdef __cplusplus
}
#endif

#endif /* SF2_LOADER_WEB_H */

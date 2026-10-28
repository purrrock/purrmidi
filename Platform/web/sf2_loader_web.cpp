/**
 * @file    sf2_loader_web.cpp
 * @brief   Веб-реализация API sf2_loader.h для WebAssembly-демо.
 *
 * На прошивке банк лежит в QSPI Flash (XIP), на ПК его читает Platform/host/sf2_loader_host.cpp из файла.
 * В браузере файлов нет: JavaScript кладёт банк прямо в память модуля (pm_sf2_alloc() + pm_sf2_commit(),
 * см. purrmidi_web.cpp), а синтезатор читает его по указателю через те же SF2Loader_Ensure()/SF2Loader_GetBank().
 */
#include "sf2_loader.h"
#include "sf2_loader_web.h"

#include <cstdlib>

namespace {

uint8_t       *g_bank = nullptr;
uint32_t       g_size = 0;
bool           g_ready = false;
SF2L_Report    g_report{};

} // namespace

extern "C" {

uint8_t *SF2LoaderWeb_Alloc(uint32_t size)
{
    SF2LoaderWeb_Unload();
    if (size == 0U) return nullptr;
    g_bank = static_cast<uint8_t *>(std::malloc(size));
    if (g_bank) g_size = size;
    return g_bank;
}

void SF2LoaderWeb_Unload(void)
{
    std::free(g_bank);
    g_bank = nullptr;
    g_size = 0;
    g_ready = false;
    g_report = SF2L_Report{};
}

bool SF2LoaderWeb_Commit(void)
{
    g_ready = false;
    if (!g_bank || g_size < SF2L_BANK_MIN_SIZE) return false;
    /* Минимальная проверка: «RIFF» ... «sfbk». Полный разбор делает TinySoundFont. */
    if (g_bank[0] != 'R' || g_bank[1] != 'I' || g_bank[2] != 'F' || g_bank[3] != 'F') return false;
    if (g_bank[8] != 's' || g_bank[9] != 'f' || g_bank[10] != 'b' || g_bank[11] != 'k') return false;

    g_report.result      = SF2L_OK_UNCHANGED;
    g_report.file_status = SF2L_FILE_NOT_CHECKED;
    g_report.base        = g_bank;
    g_report.size        = g_size;
    g_report.crc32       = 0;
    g_ready = true;
    return true;
}

bool SF2Loader_Ensure(void)
{
    return g_ready;
}

bool SF2Loader_GetBank(const uint8_t **base, uint32_t *size)
{
    if (!g_ready || !base || !size) return false;
    *base = g_bank;
    *size = g_size;
    return true;
}

bool SF2Loader_BankReadable(void)
{
    return g_ready;
}

const SF2L_Report *SF2Loader_GetReport(void)
{
    return &g_report;
}

} // extern "C"

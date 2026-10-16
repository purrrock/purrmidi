#include "sf2_loader_host.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> g_flash;        /* «QSPI Flash» хоста: файл банка целиком */
SF2L_Report          g_report{};
bool                 g_done = false; /* сценарий уже выполнялся (результат запомнен) */
bool                 g_ok   = false;
volatile bool        g_readable = true;   /* имитация открытого/закрытого окна XIP */
std::string          g_path;         /* явный путь (SF2LoaderHost_SetPath) */
bool                 g_have_path = false;

bool read_file(const char *path)
{
    std::FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    g_flash.resize(static_cast<size_t>(n));
    const size_t got = std::fread(g_flash.data(), 1, g_flash.size(), f);
    std::fclose(f);
    if (got != g_flash.size()) { g_flash.clear(); return false; }
    return true;
}

} // namespace

extern "C" {

void SF2LoaderHost_Unload(void)
{
    g_flash.clear();
    g_flash.shrink_to_fit();
    g_report = SF2L_Report{};
    g_done = false;
    g_ok = false;
    g_readable = true;
}

void SF2LoaderHost_SetReadable(bool readable)
{
    g_readable = readable;
}

void SF2LoaderHost_SetPath(const char *path)
{
    SF2LoaderHost_Unload();
    g_have_path = (path != nullptr);
    g_path = path ? path : "";
}

bool SF2Loader_Ensure(void)
{
    if (g_done) return g_ok;
    g_done = true;
    g_ok = false;

    bool loaded = false;
    if (g_have_path) {
        loaded = read_file(g_path.c_str());
        if (!loaded) std::fprintf(stderr, "[SF2] Cannot open '%s'.\n", g_path.c_str());
    } else {
        const char *env = std::getenv("PURRMIDI_TEST_SF2");
        const char *candidates[] = {
            env, "tests/SNDFNT.SF2", "SNDFNT.SF2", "SoundFonts/Yamaha S90ES lite.SF2"
        };
        for (const char *c : candidates) {
            if (c && *c && read_file(c)) { loaded = true; break; }
        }
        if (!loaded) {
            std::fprintf(stderr, "[SF2] No SoundFont found: set PURRMIDI_TEST_SF2 or put tests/SNDFNT.SF2 / SNDFNT.SF2 "
                                 "in the working directory.\n");
        }
    }
    if (!loaded) return false;

    g_report.result      = SF2L_OK_UNCHANGED;
    g_report.file_status = SF2L_FILE_NOT_CHECKED;
    g_report.base        = g_flash.data();
    g_report.size        = static_cast<uint32_t>(g_flash.size());
    g_report.crc32       = 0;
    g_ok = true;
    return true;
}

bool SF2Loader_GetBank(const uint8_t **base, uint32_t *size)
{
    if (!g_ok || !base || !size) return false;
    *base = g_flash.data();
    *size = static_cast<uint32_t>(g_flash.size());
    return true;
}

bool SF2Loader_BankReadable(void)
{
    return g_ok && g_readable;
}

const SF2L_Report *SF2Loader_GetReport(void)
{
    return &g_report;
}

} // extern "C"

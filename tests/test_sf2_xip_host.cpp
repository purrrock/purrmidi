/**
 * Хост-тест XIP-бэкенда SoundFont (Engine/synth/sf2_xip.cpp): банк берётся из памяти (имитация окна
 * 0x90000000), TinySoundFont парсит метаданные через tsf_stream поверх памяти и читает PCM по указателю.
 * Тест запускается из корня репозитория; без tests/SNDFNT.SF2 проверки с реальным банком пропускаются.
 * Режим "--hash" печатает хеш выхода: тот же рендер на файловом бэкенде (sf2_cache.cpp) даёт тот же хеш.
 */
#include "sf2_cache.h"
#include "soundfont_synth.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

#ifdef PURRMIDI_TEST_XIP_STUB
/* Заглушка загрузчика: "флеш" = файл, прочитанный в память. */
#include "sf2_loader.h"
static std::vector<uint8_t> g_flash;
static SF2L_Report g_report;
extern "C" bool SF2Loader_Ensure(void)
{
    if (!g_flash.empty()) return true;
    FILE *f = std::fopen("tests/SNDFNT.SF2", "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    g_flash.resize((size_t)n);
    size_t got = std::fread(g_flash.data(), 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n) { g_flash.clear(); return false; }
    g_report.base = g_flash.data();
    g_report.size = (uint32_t)n;
    return true;
}
extern "C" bool SF2Loader_GetBank(const uint8_t **base, uint32_t *size)
{
    if (g_flash.empty()) return false;
    *base = g_flash.data(); *size = (uint32_t)g_flash.size();
    return true;
}
extern "C" const SF2L_Report *SF2Loader_GetReport(void) { return &g_report; }
#endif

static uint64_t fnv(uint64_t h, const int16_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) { h ^= (uint16_t)p[i]; h *= 1099511628211ULL; }
    return h;
}

int main(int argc, char **argv)
{
    const bool hash_mode = (argc > 1 && std::strcmp(argv[1], "--hash") == 0);

    if (!SoundFontSynth_InitSF2()) {
        std::puts("test_sf2_xip_host: tests/SNDFNT.SF2 not available, skipped");
        return 0;
    }
    CHECK(SoundFontSynth_IsLoaded());
    CHECK(SF2Cache_GetSmplFileOffset() > 0);

    std::vector<int16_t> buf(512 * 2);
    uint64_t h = 1469598103934665603ULL;
    int peak = 0;

    SoundFontSynth_NoteOn(60, 100);
    SoundFontSynth_NoteOn(64, 90);
    SoundFontSynth_NoteOn(67, 80);
    for (int b = 0; b < 40; ++b) {
        SoundFontSynth_FillStereoBuffer(buf.data(), 512);
        SoundFontSynth_Process();
        h = fnv(h, buf.data(), buf.size());
        for (int16_t s : buf) { int a = s < 0 ? -s : s; if (a > peak) peak = a; }
        if (b == 20) { SoundFontSynth_NoteOff(60); SoundFontSynth_NoteOff(64); SoundFontSynth_NoteOff(67); }
    }
    CHECK(peak > 500);

    SF2CacheStats st;
    SF2Cache_GetStats(&st);
    CHECK(st.miss_samples == 0);

    if (hash_mode) std::printf("hash=%016llx peak=%d\n", (unsigned long long)h, peak);
    if (g_failures) { std::fprintf(stderr, "%d check(s) FAILED\n", g_failures); return 1; }
    std::puts("test_sf2_xip_host: all checks passed");
    return 0;
}

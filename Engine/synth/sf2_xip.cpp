/**
 * @file    sf2_xip.cpp
 * @brief   Бэкенд SF2Cache для прошивки: сэмплы читаются напрямую из QSPI Flash (XIP, 0x90000000).
 *
 * Заменяет sf2_cache.cpp в сборке прошивки (CMake выбирает файл, когда проект собран с QSPI-XIP, см.
 * CMakeLists.txt, PURRMIDI_SF2_XIP). Публичный API (sf2_cache.h) сохранён, поэтому soundfont_synth.cpp,
 * main.c и TinySoundFont работают без изменений, но:
 *   - никакого RAM-кэша, очереди запросов и чтения с опережением: PCM-отсчёт - это обычное чтение
 *     int16_t по указателю в XIP-окне; быстрым его делает аппаратный D-cache (см. xip_mpu.h);
 *   - метаданные SF2 (пресеты, инструменты, регионы) парсятся из того же XIP-окна и лежат в куче;
 *   - SD-карта здесь не используется вообще: банк туда попадает только при обновлении (sf2_loader.c).
 * Хост-сборки (Windows/Linux) по-прежнему используют sf2_cache.cpp с файловым бэкендом.
 */
#include "sf2_cache.h"
#include "sf2_loader.h"

#include <cstdio>
#include <cstring>

namespace {

const uint8_t *g_bank       = nullptr;   /* начало банка в XIP */
uint32_t       g_bank_size  = 0;
uint32_t       g_pos        = 0;         /* позиция чтения для tsf_stream */
bool           g_open       = false;

const int16_t *g_smpl       = nullptr;   /* PCM-данные чанка smpl */
uint32_t       g_smpl_total = 0;         /* число отсчётов */
uint32_t       g_smpl_off   = 0;

/* tsf_stream поверх XIP-окна */
int stream_read_cb(void *, void *ptr, unsigned int size)
{
    if (!g_open || g_pos >= g_bank_size) return 0;
    const uint32_t avail = g_bank_size - g_pos;
    const uint32_t n = (size < avail) ? size : avail;
    std::memcpy(ptr, g_bank + g_pos, n);
    g_pos += n;
    return (int)n;
}

int stream_skip_cb(void *, unsigned int count)
{
    if (!g_open) return 0;
    if ((uint64_t)g_pos + count > g_bank_size) return 0;
    g_pos += count;
    return 1;
}

unsigned int stream_tell_cb(void *)
{
    return g_open ? g_pos : 0U;
}

} // namespace

extern "C" {

bool SF2Cache_OpenFile(const char * /*filepath*/)
{
    SF2Cache_CloseFile();

    /* Сценарий загрузки (QSPI init, SD-обновление, XIP) выполняется один раз; путь на SD задаёт SF2_SD_PATH. */
    if (!SF2Loader_Ensure() || !SF2Loader_GetBank(&g_bank, &g_bank_size)) {
        std::fprintf(stderr, "[SF2] No SoundFont bank in QSPI flash\n");
        return false;
    }
    g_pos  = 0;
    g_open = true;
    return true;
}

void SF2Cache_CloseFile(void)
{
    g_open       = false;
    g_pos        = 0;
    g_smpl       = nullptr;
    g_smpl_total = 0;
    g_smpl_off   = 0;
}

bool SF2Cache_IsFileOpen(void)
{
    return g_open;
}

void SF2Cache_Reset(void) {}
void SF2Cache_BeginAudioBlock(void) {}
void SF2Cache_ResetProfile(void) {}
void SF2Cache_ProcessRequests(void) {}
void SF2Cache_RequestBlock(uint32_t) {}
void SF2Cache_ResetStats(void) {}

bool SF2Cache_PreloadBlock(uint32_t)
{
    return true;   /* XIP: данные всегда "на месте" */
}

void SF2Cache_GetProfile(SF2Profile *out)
{
    if (out) std::memset(out, 0, sizeof(*out));
}

void SF2Cache_GetStats(SF2CacheStats *out)
{
    if (out) std::memset(out, 0, sizeof(*out));
}

void SF2Cache_InitStream(struct tsf_stream *stream)
{
    if (!stream) return;
    stream->data = nullptr;
    stream->read = stream_read_cb;
    stream->skip = stream_skip_cb;
    stream->tell = stream_tell_cb;
}

uint32_t SF2Cache_GetSmplFileOffset(void)
{
    return g_smpl_off;
}

void SF2Cache_SetSmplFileOffset(uint32_t file_offset, uint32_t total_samples)
{
    g_smpl_off   = file_offset;
    g_smpl       = nullptr;
    g_smpl_total = 0;

    if (!g_bank || (file_offset & 1U) != 0U || file_offset >= g_bank_size) {
        std::fprintf(stderr, "[SF2] Bad smpl chunk offset %lu\n", (unsigned long)file_offset);
        return;
    }
    const uint32_t max_samples = (g_bank_size - file_offset) / 2U;   /* не выходим за пределы банка */
    g_smpl       = reinterpret_cast<const int16_t *>(g_bank + file_offset);
    g_smpl_total = (total_samples < max_samples) ? total_samples : max_samples;
}

short SF2Cache_GetSample(tsf *, uint32_t sample_index, int *)
{
    return (sample_index < g_smpl_total) ? (short)g_smpl[sample_index] : (short)0;
}

/* Хук, который вызывает TinySoundFont (tsf.h) на каждый отсчёт каждого голоса. */
short tsf_get_sample_pcm16(tsf *, unsigned int pos, int *)
{
    return (pos < g_smpl_total) ? (short)g_smpl[pos] : (short)0;
}

} // extern "C"

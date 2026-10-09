/**
 * @file    sf2_cache.cpp
 * @brief   Bounded RAM PCM sample cache and platform file backend implementation.
 */
#include "sf2_cache.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(PURRMIDI_HOST_BUILD) || defined(WIN32) || defined(__unix__) || defined(__APPLE__)
#define SF2_HOST_MODE 1
#else
#define SF2_HOST_MODE 0
#include "ff.h"
#include "sd_storage.h"
#endif

namespace {

struct CacheBlock {
    std::atomic<uint32_t> block_index{0xFFFFFFFFU};
    std::atomic<uint32_t> state{SF2_BLOCK_EMPTY};
    uint32_t last_used_tick{0};
    int16_t samples[SF2_BLOCK_SAMPLES];
};

#define REQ_QUEUE_SIZE 64U
struct ReqQueue {
    std::atomic<uint32_t> head{0};
    std::atomic<uint32_t> tail{0};
    uint32_t items[REQ_QUEUE_SIZE];
};

CacheBlock g_cache[SF2_CACHE_BLOCKS];
ReqQueue   g_req_queue;

std::atomic<uint32_t> g_lru_clock{0};
uint32_t g_smpl_file_offset = 0;
uint32_t g_smpl_total_samples = 0;

#if SF2_HOST_MODE
FILE *g_host_file = nullptr;
#else
FIL   g_fatfs_file;
bool  g_fatfs_open = false;
#endif

/* Stream callbacks for tsf_load */
int stream_read_cb(void *data, void *ptr, unsigned int size)
{
    (void)data;
#if SF2_HOST_MODE
    if (!g_host_file) return 0;
    return (int)std::fread(ptr, 1, size, g_host_file);
#else
    if (!g_fatfs_open) return 0;
    UINT bytes_read = 0;
    if (f_read(&g_fatfs_file, ptr, (UINT)size, &bytes_read) == FR_OK) {
        return (int)bytes_read;
    }
    return 0;
#endif
}

int stream_skip_cb(void *data, unsigned int count)
{
    (void)data;
#if SF2_HOST_MODE
    if (!g_host_file) return 0;
    return std::fseek(g_host_file, (long)count, SEEK_CUR) == 0;
#else
    if (!g_fatfs_open) return 0;
    FSIZE_t curr = f_tell(&g_fatfs_file);
    return f_lseek(&g_fatfs_file, curr + count) == FR_OK;
#endif
}

unsigned int stream_tell_cb(void *data)
{
    (void)data;
#if SF2_HOST_MODE
    if (!g_host_file) return 0;
    return (unsigned int)std::ftell(g_host_file);
#else
    if (!g_fatfs_open) return 0;
    return (unsigned int)f_tell(&g_fatfs_file);
#endif
}

} // namespace

extern "C" {

bool SF2Cache_OpenFile(const char *filepath)
{
    SF2Cache_CloseFile();

#if SF2_HOST_MODE
    g_host_file = std::fopen(filepath, "rb");
    if (!g_host_file && std::strcmp(filepath, "0:/SNDFNT.SF2") == 0) {
        g_host_file = std::fopen("tests/SNDFNT.SF2", "rb");
        if (!g_host_file) {
            g_host_file = std::fopen("SNDFNT.SF2", "rb");
        }
    }
    return g_host_file != nullptr;
#else
    if (!SDStorage_IsMounted()) {
        if (SDStorage_Mount() != SDS_OK) {
            return false;
        }
    }
    if (f_open(&g_fatfs_file, filepath, FA_READ) == FR_OK) {
        g_fatfs_open = true;
        return true;
    }
    return false;
#endif
}

void SF2Cache_CloseFile(void)
{
#if SF2_HOST_MODE
    if (g_host_file) {
        std::fclose(g_host_file);
        g_host_file = nullptr;
    }
#else
    if (g_fatfs_open) {
        f_close(&g_fatfs_file);
        g_fatfs_open = false;
    }
#endif
    SF2Cache_Reset();
}

bool SF2Cache_IsFileOpen(void)
{
#if SF2_HOST_MODE
    return g_host_file != nullptr;
#else
    return g_fatfs_open;
#endif
}

void SF2Cache_Reset(void)
{
    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
        g_cache[i].state.store(SF2_BLOCK_EMPTY, std::memory_order_relaxed);
        g_cache[i].block_index.store(0xFFFFFFFFU, std::memory_order_relaxed);
        g_cache[i].last_used_tick = 0;
        std::memset(g_cache[i].samples, 0, sizeof(g_cache[i].samples));
    }
    g_req_queue.head.store(0, std::memory_order_relaxed);
    g_req_queue.tail.store(0, std::memory_order_relaxed);
    g_lru_clock.store(0, std::memory_order_relaxed);
}

void SF2Cache_InitStream(struct tsf_stream *stream)
{
    if (!stream) return;
    stream->data = nullptr;
    stream->read = stream_read_cb;
    stream->skip = stream_skip_cb;
    stream->tell = stream_tell_cb;
}

void SF2Cache_RequestBlock(uint32_t block_index)
{
    if (block_index == 0xFFFFFFFFU) return;

    /* Check if already in cache */
    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
        uint32_t st = g_cache[i].state.load(std::memory_order_relaxed);
        if (st != SF2_BLOCK_EMPTY && g_cache[i].block_index.load(std::memory_order_relaxed) == block_index) {
            return;
        }
    }

    uint32_t head = g_req_queue.head.load(std::memory_order_relaxed);
    uint32_t tail = g_req_queue.tail.load(std::memory_order_acquire);

    if ((head - tail) >= REQ_QUEUE_SIZE) {
        return; /* queue full */
    }

    /* Check if already in queue */
    for (uint32_t i = tail; i != head; ++i) {
        if (g_req_queue.items[i % REQ_QUEUE_SIZE] == block_index) {
            return;
        }
    }

    g_req_queue.items[head % REQ_QUEUE_SIZE] = block_index;
    g_req_queue.head.store(head + 1, std::memory_order_release);
}

void SF2Cache_ProcessRequests(void)
{
    if (!SF2Cache_IsFileOpen()) return;

    uint32_t tail = g_req_queue.tail.load(std::memory_order_relaxed);
    uint32_t head = g_req_queue.head.load(std::memory_order_acquire);

    if (tail == head) return; /* empty queue */

    uint32_t block_idx = g_req_queue.items[tail % REQ_QUEUE_SIZE];
    g_req_queue.tail.store(tail + 1, std::memory_order_release);

    /* Double check if already loaded */
    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
        if (g_cache[i].state.load(std::memory_order_relaxed) == SF2_BLOCK_READY &&
            g_cache[i].block_index.load(std::memory_order_relaxed) == block_idx) {
            return;
        }
    }

    /* Find eviction slot (EMPTY or LRU READY) */
    uint32_t target_slot = 0;
    uint32_t oldest_tick = 0xFFFFFFFFU;

    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
        uint32_t st = g_cache[i].state.load(std::memory_order_relaxed);
        if (st == SF2_BLOCK_EMPTY) {
            target_slot = i;
            break;
        }
        if (st == SF2_BLOCK_READY && g_cache[i].last_used_tick < oldest_tick) {
            oldest_tick = g_cache[i].last_used_tick;
            target_slot = i;
        }
    }

    /* Transition target slot to LOADING and invalidate block_index atomically with release barrier */
    g_cache[target_slot].block_index.store(0xFFFFFFFFU, std::memory_order_release);
    g_cache[target_slot].state.store(SF2_BLOCK_LOADING, std::memory_order_release);

    /* Read PCM block from file */
    uint32_t file_offset = g_smpl_file_offset + block_idx * (SF2_BLOCK_SAMPLES * 2U);
    uint32_t samples_left = (block_idx * SF2_BLOCK_SAMPLES < g_smpl_total_samples)
                            ? (g_smpl_total_samples - block_idx * SF2_BLOCK_SAMPLES)
                            : 0U;
    uint32_t samples_to_read = (samples_left > SF2_BLOCK_SAMPLES) ? SF2_BLOCK_SAMPLES : samples_left;
    uint32_t bytes_to_read = samples_to_read * 2U;

    std::memset(g_cache[target_slot].samples, 0, sizeof(g_cache[target_slot].samples));

    if (bytes_to_read > 0) {
#if SF2_HOST_MODE
        if (g_host_file) {
            std::fseek(g_host_file, (long)file_offset, SEEK_SET);
            std::fread(g_cache[target_slot].samples, 1, bytes_to_read, g_host_file);
        }
#else
        if (g_fatfs_open) {
            UINT br = 0;
            if (f_lseek(&g_fatfs_file, (FSIZE_t)file_offset) == FR_OK) {
                f_read(&g_fatfs_file, g_cache[target_slot].samples, (UINT)bytes_to_read, &br);
            }
        }
#endif
    }

    g_cache[target_slot].block_index.store(block_idx, std::memory_order_relaxed);
    g_cache[target_slot].last_used_tick = g_lru_clock.load(std::memory_order_relaxed);
    g_cache[target_slot].state.store(SF2_BLOCK_READY, std::memory_order_release);
}

short SF2Cache_GetSample(tsf *f, uint32_t sample_index, int *last_slot_hint)
{
    if (!f || sample_index >= tsf_get_smpl_sample_count(f)) {
        return 0;
    }

    uint32_t block_idx = sample_index / SF2_BLOCK_SAMPLES;
    uint32_t sample_offset = sample_index % SF2_BLOCK_SAMPLES;
    uint32_t tick = g_lru_clock.fetch_add(1, std::memory_order_relaxed);

    /* Check hint slot first */
    if (last_slot_hint && *last_slot_hint >= 0 && *last_slot_hint < (int)SF2_CACHE_BLOCKS) {
        uint32_t slot = (uint32_t)*last_slot_hint;
        if (g_cache[slot].state.load(std::memory_order_acquire) == SF2_BLOCK_READY &&
            g_cache[slot].block_index.load(std::memory_order_acquire) == block_idx) {
            g_cache[slot].last_used_tick = tick;
            return g_cache[slot].samples[sample_offset];
        }
    }

    /* Linear search across cache slots */
    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
        if (g_cache[i].state.load(std::memory_order_acquire) == SF2_BLOCK_READY &&
            g_cache[i].block_index.load(std::memory_order_acquire) == block_idx) {
            if (last_slot_hint) {
                *last_slot_hint = (int)i;
            }
            g_cache[i].last_used_tick = tick;
            return g_cache[i].samples[sample_offset];
        }
    }

    /* Cache miss: request current block and next block */
    SF2Cache_RequestBlock(block_idx);
    SF2Cache_RequestBlock(block_idx + 1);

    return 0;
}

uint32_t SF2Cache_GetSmplFileOffset(void)
{
    return g_smpl_file_offset;
}

void SF2Cache_SetSmplFileOffset(uint32_t file_offset, uint32_t total_samples)
{
    g_smpl_file_offset = file_offset;
    g_smpl_total_samples = total_samples;
}

short tsf_get_sample_pcm16(tsf *f, unsigned int pos, int *last_slot_hint)
{
    return SF2Cache_GetSample(f, (uint32_t)pos, last_slot_hint);
}

} // extern "C"

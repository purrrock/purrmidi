/**
 * @file    sf2_cache.cpp
 * @brief   Bounded RAM PCM sample cache and platform file backend implementation.
 */
#include "sf2_cache.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#if !SF2_HOST_MODE
#include "ff.h"
#include "sd_storage.h"
#endif

namespace {

struct CacheBlock {
    std::atomic<uint32_t> block_index{0xFFFFFFFFU};
    std::atomic<uint32_t> state{SF2_BLOCK_EMPTY};
    std::atomic<uint32_t> active_readers{0};
    std::atomic<uint32_t> touched{0};   /* set by audio thread on every hit; consumed by read-ahead */
    std::atomic<uint32_t> last_used_tick{0};   /* written by audio, read by main (LRU) */
    int16_t samples[SF2_BLOCK_SAMPLES];
};

#if !SF2_HOST_MODE
/* Keep the large PCM cache in DMA-accessible SRAM_D2, not the limited DTCM. */
#define SF2_CACHE_SECTION __attribute__((section(".ram_d2"), aligned(32)))
#else
#define SF2_CACHE_SECTION
#endif

#define REQ_QUEUE_SIZE 64U
struct ReqQueue {
    std::atomic<uint32_t> head{0};
    std::atomic<uint32_t> tail{0};
    uint32_t items[REQ_QUEUE_SIZE];
};

SF2_CACHE_SECTION CacheBlock g_cache[SF2_CACHE_BLOCKS];
ReqQueue   g_req_queue;

std::atomic<uint32_t> g_lru_clock{0};
uint32_t g_smpl_file_offset = 0;
uint32_t g_smpl_total_samples = 0;

std::atomic<uint32_t> g_stat_miss_samples{0};
std::atomic<uint32_t> g_stat_blocks_loaded{0};
std::atomic<uint32_t> g_stat_load_errors{0};

enum class LoadResult {
    Skipped,   /* already resident, or beyond the end of the sample data: nothing was read */
    Loaded,    /* block read from file and published                                      */
    Failed,    /* file read failed or was short: nothing published                         */
    NoSlot     /* every candidate slot is in use by an audio reader                        */
};

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
        if (!g_host_file) {
            std::fprintf(stderr, "[SF2] Cannot open '%s', 'tests/SNDFNT.SF2', or 'SNDFNT.SF2'.\n", filepath);
            std::fprintf(stderr, "[SF2] Host paths are relative to the process working directory.\n");
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
        g_cache[i].active_readers.store(0, std::memory_order_relaxed);
        g_cache[i].touched.store(0, std::memory_order_relaxed);
        g_cache[i].last_used_tick.store(0, std::memory_order_relaxed);
        std::memset(g_cache[i].samples, 0, sizeof(g_cache[i].samples));
    }
    g_req_queue.head.store(0, std::memory_order_relaxed);
    g_req_queue.tail.store(0, std::memory_order_relaxed);
    g_lru_clock.store(0, std::memory_order_relaxed);
}

static bool try_read_slot(uint32_t slot, uint32_t block_idx, uint32_t sample_offset, uint32_t tick, int16_t *out_sample)
{
    if (g_cache[slot].state.load(std::memory_order_seq_cst) != SF2_BLOCK_READY ||
        g_cache[slot].block_index.load(std::memory_order_seq_cst) != block_idx) {
        return false;
    }

    g_cache[slot].active_readers.fetch_add(1, std::memory_order_seq_cst);

    if (g_cache[slot].state.load(std::memory_order_seq_cst) == SF2_BLOCK_READY &&
        g_cache[slot].block_index.load(std::memory_order_seq_cst) == block_idx) {
        *out_sample = g_cache[slot].samples[sample_offset];
        g_cache[slot].last_used_tick.store(tick, std::memory_order_relaxed);
        if (g_cache[slot].touched.load(std::memory_order_relaxed) == 0) {
            g_cache[slot].touched.store(1, std::memory_order_relaxed);   /* triggers read-ahead */
        }
        g_cache[slot].active_readers.fetch_sub(1, std::memory_order_seq_cst);
        return true;
    }

    g_cache[slot].active_readers.fetch_sub(1, std::memory_order_seq_cst);
    return false;
}

static bool try_claim_slot(uint32_t slot)
{
    uint32_t st = g_cache[slot].state.load(std::memory_order_seq_cst);
    if (st != SF2_BLOCK_EMPTY && st != SF2_BLOCK_READY) {
        return false;
    }

    g_cache[slot].state.store(SF2_BLOCK_EVICTING, std::memory_order_seq_cst);

    if (g_cache[slot].active_readers.load(std::memory_order_seq_cst) > 0) {
        g_cache[slot].state.store(st, std::memory_order_release);
        return false;
    }

    return true;
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
    if ((uint64_t)block_index * SF2_BLOCK_SAMPLES >= g_smpl_total_samples) return;   /* past the end */

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

/* Returns the slot holding block_idx in READY state, or -1. */
static int find_ready_slot(uint32_t block_idx)
{
    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
        if (g_cache[i].state.load(std::memory_order_seq_cst) == SF2_BLOCK_READY &&
            g_cache[i].block_index.load(std::memory_order_seq_cst) == block_idx) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Main context only. Makes sure block_idx is resident: picks an EMPTY or least recently used
 * slot that has no audio readers and reads the block from the file into it.
 */
static LoadResult load_block(uint32_t block_idx)
{
    if ((uint64_t)block_idx * SF2_BLOCK_SAMPLES >= g_smpl_total_samples) {
        return LoadResult::Skipped;
    }
    if (find_ready_slot(block_idx) >= 0) {
        return LoadResult::Skipped;
    }

    /* Find eviction slot (EMPTY or LRU READY) free of active readers */
    int target_slot = -1;

    /* First pass: try EMPTY slots */
    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
        if (g_cache[i].state.load(std::memory_order_seq_cst) == SF2_BLOCK_EMPTY) {
            if (try_claim_slot(i)) {
                target_slot = (int)i;
                break;
            }
        }
    }

    /* Second pass: if no EMPTY slot claimed, try READY slots in LRU order */
    if (target_slot < 0) {
        bool slot_tested[SF2_CACHE_BLOCKS] = {false};
        for (uint32_t pass = 0; pass < SF2_CACHE_BLOCKS; ++pass) {
            uint32_t oldest_tick = 0xFFFFFFFFU;
            int candidate = -1;

            for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
                if (!slot_tested[i] &&
                    g_cache[i].state.load(std::memory_order_seq_cst) == SF2_BLOCK_READY) {
                    uint32_t tick_i = g_cache[i].last_used_tick.load(std::memory_order_relaxed);
                    if (tick_i < oldest_tick) {
                        oldest_tick = tick_i;
                        candidate = (int)i;
                    }
                }
            }

            if (candidate < 0) {
                break;
            }

            slot_tested[candidate] = true;
            if (try_claim_slot((uint32_t)candidate)) {
                target_slot = candidate;
                break;
            }
        }
    }

    if (target_slot < 0) {
        return LoadResult::NoSlot;
    }

    uint32_t slot = (uint32_t)target_slot;

    /* Transition target slot to LOADING and invalidate block_index atomically with release barrier */
    g_cache[slot].block_index.store(0xFFFFFFFFU, std::memory_order_release);
    g_cache[slot].state.store(SF2_BLOCK_LOADING, std::memory_order_release);
    g_cache[slot].touched.store(0, std::memory_order_relaxed);

    /* Read PCM block from file */
    uint32_t file_offset = g_smpl_file_offset + block_idx * (SF2_BLOCK_SAMPLES * 2U);
    uint32_t samples_left = g_smpl_total_samples - block_idx * SF2_BLOCK_SAMPLES;   /* > 0, checked above */
    uint32_t samples_to_read = (samples_left > SF2_BLOCK_SAMPLES) ? SF2_BLOCK_SAMPLES : samples_left;
    uint32_t bytes_to_read = samples_to_read * 2U;

    std::memset(g_cache[slot].samples, 0, sizeof(g_cache[slot].samples));

    bool read_success = false;

#if SF2_HOST_MODE
    if (g_host_file) {
        std::clearerr(g_host_file);
        if (std::fseek(g_host_file, (long)file_offset, SEEK_SET) == 0) {
            size_t bytes_read = std::fread(g_cache[slot].samples, 1, bytes_to_read, g_host_file);
            if (bytes_read == (size_t)bytes_to_read && std::ferror(g_host_file) == 0) {
                read_success = true;
            }
        }
    }
#else
    if (g_fatfs_open) {
        UINT br = 0;
        if (f_lseek(&g_fatfs_file, (FSIZE_t)file_offset) == FR_OK) {
            if (f_read(&g_fatfs_file, g_cache[slot].samples, (UINT)bytes_to_read, &br) == FR_OK) {
                if ((uint32_t)br == bytes_to_read) {
                    read_success = true;
                }
            }
        }
    }
#endif

    if (read_success) {
        g_cache[slot].block_index.store(block_idx, std::memory_order_release);
        g_cache[slot].last_used_tick.store(g_lru_clock.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_cache[slot].state.store(SF2_BLOCK_READY, std::memory_order_release);
        g_stat_blocks_loaded.fetch_add(1, std::memory_order_relaxed);
        return LoadResult::Loaded;
    }

    g_cache[slot].block_index.store(0xFFFFFFFFU, std::memory_order_release);
    g_cache[slot].state.store(SF2_BLOCK_EMPTY, std::memory_order_release);
    g_stat_load_errors.fetch_add(1, std::memory_order_relaxed);
    return LoadResult::Failed;
}

void SF2Cache_ProcessRequests(void)
{
    if (!SF2Cache_IsFileOpen()) return;

    uint32_t budget = SF2_MAX_BLOCKS_PER_PROCESS;

    /*
     * 1. Blocks the audio thread already missed (urgent: it is playing silence right now).
     *    Only requests queued before this call are serviced; a block that fails to load is
     *    re-queued behind that snapshot and retried by the NEXT call, so a persistent read
     *    error can never keep this function spinning.
     */
    uint32_t tail = g_req_queue.tail.load(std::memory_order_relaxed);
    const uint32_t head = g_req_queue.head.load(std::memory_order_acquire);

    while (tail != head && budget > 0) {
        uint32_t block_idx = g_req_queue.items[tail % REQ_QUEUE_SIZE];
        ++tail;
        g_req_queue.tail.store(tail, std::memory_order_release);

        switch (load_block(block_idx)) {
            case LoadResult::Loaded:
                --budget;
                break;
            case LoadResult::Failed:
                --budget;
                SF2Cache_RequestBlock(block_idx);   /* retry on the next call */
                break;
            case LoadResult::NoSlot:
                SF2Cache_RequestBlock(block_idx);   /* all candidates busy, retry later */
                break;
            case LoadResult::Skipped:
                break;
        }
    }

    /*
     * 2. Read-ahead. Every block the audio thread read since the last scan is flagged
     *    "touched"; make sure the next SF2_READAHEAD_BLOCKS blocks are resident before the
     *    voice gets there, so block boundaries never turn into holes of silence.
     */
    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS && budget > 0; ++i) {
        CacheBlock &cb = g_cache[i];
        if (cb.touched.load(std::memory_order_relaxed) == 0) continue;
        if (cb.state.load(std::memory_order_seq_cst) != SF2_BLOCK_READY) continue;

        uint32_t base = cb.block_index.load(std::memory_order_seq_cst);
        if (base == 0xFFFFFFFFU) continue;

        cb.touched.store(0, std::memory_order_relaxed);

        for (uint32_t k = 1; k <= SF2_READAHEAD_BLOCKS; ++k) {
            if (budget == 0) {
                cb.touched.store(1, std::memory_order_relaxed);   /* finish on the next call */
                break;
            }
            LoadResult r = load_block(base + k);
            if (r == LoadResult::Loaded || r == LoadResult::Failed) {
                --budget;
            }
        }
    }
}

bool SF2Cache_PreloadBlock(uint32_t block_index)
{
    if (!SF2Cache_IsFileOpen()) return false;

    LoadResult r = load_block(block_index);
    return (r == LoadResult::Loaded) || (r == LoadResult::Skipped && find_ready_slot(block_index) >= 0);
}

short SF2Cache_GetSample(tsf *f, uint32_t sample_index, int *last_slot_hint)
{
    if (!f || sample_index >= tsf_get_smpl_sample_count(f)) {
        return 0;
    }

    uint32_t block_idx = sample_index / SF2_BLOCK_SAMPLES;
    uint32_t sample_offset = sample_index % SF2_BLOCK_SAMPLES;
    uint32_t tick = g_lru_clock.fetch_add(1, std::memory_order_relaxed);

    int16_t sample = 0;

    /* Check hint slot first */
    if (last_slot_hint && *last_slot_hint >= 0 && *last_slot_hint < (int)SF2_CACHE_BLOCKS) {
        uint32_t slot = (uint32_t)*last_slot_hint;
        if (try_read_slot(slot, block_idx, sample_offset, tick, &sample)) {
            return sample;
        }
    }

    /* Linear search across cache slots */
    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
        if (try_read_slot(i, block_idx, sample_offset, tick, &sample)) {
            if (last_slot_hint) {
                *last_slot_hint = (int)i;
            }
            return sample;
        }
    }

    /* Cache miss: request current block and next block */
    g_stat_miss_samples.fetch_add(1, std::memory_order_relaxed);
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

void SF2Cache_GetStats(SF2CacheStats *out)
{
    if (!out) return;
    out->miss_samples  = g_stat_miss_samples.load(std::memory_order_relaxed);
    out->blocks_loaded = g_stat_blocks_loaded.load(std::memory_order_relaxed);
    out->load_errors   = g_stat_load_errors.load(std::memory_order_relaxed);
}

void SF2Cache_ResetStats(void)
{
    g_stat_miss_samples.store(0, std::memory_order_relaxed);
    g_stat_blocks_loaded.store(0, std::memory_order_relaxed);
    g_stat_load_errors.store(0, std::memory_order_relaxed);
}

short tsf_get_sample_pcm16(tsf *f, unsigned int pos, int *last_slot_hint)
{
    return SF2Cache_GetSample(f, (uint32_t)pos, last_slot_hint);
}

} // extern "C"

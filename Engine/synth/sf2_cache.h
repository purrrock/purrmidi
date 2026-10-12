/**
 * @file    sf2_cache.h
 * @brief   Bounded RAM PCM sample cache and file backend for TinySoundFont.
 *
 * Threading model
 * ---------------
 *  - Audio context (DMA ISR on STM32, audio callback thread on PC) only READS the cache via
 *    SF2Cache_GetSample() and never touches the file. A miss returns silence and queues a request.
 *  - Main context owns ALL file I/O: SF2Cache_ProcessRequests() and SF2Cache_PreloadBlock().
 *    They must be called from the same (main) thread, never concurrently with each other.
 *
 * Why the cache must read ahead
 * -----------------------------
 * A purely reactive cache (load a block only after the audio thread missed it) cannot be glitch
 * free: the voice keeps advancing while the block is being loaded, so every block boundary
 * becomes a hole of silence (audible as clicks/noise), and if the main loop is slower than the
 * voices consume data the sound disappears completely. Therefore:
 *   1. SF2Cache_PreloadBlock() lets the synth load the first blocks of a note BEFORE it starts;
 *   2. every block that the audio thread reads is flagged "touched" and the main loop then
 *      loads the following SF2_READAHEAD_BLOCKS blocks;
 *   3. SF2Cache_ProcessRequests() services up to SF2_MAX_BLOCKS_PER_PROCESS blocks per call.
 */
#ifndef SF2_CACHE_H
#define SF2_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "tsf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host (Windows / Linux / macOS test) build uses stdio; firmware uses FatFs. */
#ifndef SF2_HOST_MODE
#if defined(PURRMIDI_HOST_BUILD) || defined(_WIN32) || defined(WIN32) || defined(__unix__) || defined(__APPLE__)
#define SF2_HOST_MODE 1
#else
#define SF2_HOST_MODE 0
#endif
#endif

#define SF2_BLOCK_SAMPLES 1024U    /* 1024 int16_t samples = 2048 bytes per cache block */

/*
 * Number of cache blocks.
 *  - Firmware: 32 blocks x 2 KB = 64 KB of RAM (bounded by design).
 *  - Host:     256 blocks x 2 KB = 512 KB. A looped piano note keeps ~14 blocks of loop resident
 *              per sample (L and R), so 32 blocks are exhausted by a couple of notes.
 */
#ifndef SF2_CACHE_BLOCKS
#if SF2_HOST_MODE
#define SF2_CACHE_BLOCKS  256U
#else
#define SF2_CACHE_BLOCKS  32U
#endif
#endif

/* How many blocks after a block that the audio thread is reading are loaded in advance. */
#ifndef SF2_READAHEAD_BLOCKS
#define SF2_READAHEAD_BLOCKS 3U
#endif

/* Upper bound of file reads performed by one SF2Cache_ProcessRequests() call. */
#ifndef SF2_MAX_BLOCKS_PER_PROCESS
#if SF2_HOST_MODE
#define SF2_MAX_BLOCKS_PER_PROCESS 64U
#else
#define SF2_MAX_BLOCKS_PER_PROCESS 2U
#endif
#endif

typedef enum {
    SF2_BLOCK_EMPTY    = 0,
    SF2_BLOCK_LOADING  = 1,
    SF2_BLOCK_READY    = 2,
    SF2_BLOCK_EVICTING = 3
} SF2BlockState;

/** Diagnostic counters (monotonic, relaxed atomics). */
typedef struct {
    uint32_t miss_samples;     /**< samples the audio thread had to replace with silence       */
    uint32_t blocks_loaded;    /**< blocks successfully read from the file                     */
    uint32_t load_errors;      /**< failed/short block reads                                   */
} SF2CacheStats;

/** Open SF2 file using platform file backend (FatFs on firmware, stdio on host). */
/**
 * Diagnostics (cycle counts are CPU cycles of the DWT counter on the firmware, 0 on host builds).
 * Written by the audio interrupt (GetSample) and by the main loop (ProcessRequests / reads).
 */
typedef struct {
    uint32_t calls;              /**< SF2Cache_GetSample() calls that reached the cache                  */
    uint32_t hint_hits;          /**< served from the voice's remembered slot                            */
    uint32_t search_hits;        /**< served after a linear search over the slots                        */
    uint32_t misses;             /**< not resident: silence returned and blocks requested                */
    uint32_t search_slots;       /**< slots examined by all linear searches (hits and misses)            */
    uint32_t cycles_hint;        /**< total cycles spent in hint hits                                    */
    uint32_t cycles_search;      /**< total cycles spent in search hits                                  */
    uint32_t cycles_miss;        /**< total cycles spent in misses (includes the two RequestBlock calls) */
    uint32_t max_cycles;         /**< longest single call                                                */
    uint32_t proc_calls;         /**< SF2Cache_ProcessRequests() calls                                   */
    uint32_t proc_cycles_total;
    uint32_t proc_cycles_max;
    uint32_t read_calls;         /**< block reads from the card                                          */
    uint32_t read_cycles_total;
    uint32_t read_cycles_max;
} SF2Profile;

void SF2Cache_GetProfile(SF2Profile *out);

/**
 * Call once at the start of every audio block, before the first SF2Cache_GetSample() of that block.
 * It advances the LRU clock and invalidates the per-block "known missing" memo.
 */
void SF2Cache_BeginAudioBlock(void);
void SF2Cache_ResetProfile(void);

bool SF2Cache_OpenFile(const char *filepath);

/** Close open file and reset cache. */
void SF2Cache_CloseFile(void);

/** Returns true if SF2 file is currently open. */
bool SF2Cache_IsFileOpen(void);

/** Reset cache blocks and request queue. */
void SF2Cache_Reset(void);

/** Setup tsf_stream callbacks for reading SF2 metadata. */
void SF2Cache_InitStream(struct tsf_stream *stream);

/**
 * Main-loop service routine: loads blocks the audio thread missed, then reads ahead of the
 * blocks it is currently playing. Performs at most SF2_MAX_BLOCKS_PER_PROCESS file reads.
 * A block that fails to load is retried on the next call (never inside the same call).
 */
void SF2Cache_ProcessRequests(void);

/**
 * Synchronously load one block (main context only). Used by the synth to have the first
 * blocks of a note resident before the voice starts. Returns true if the block is in the
 * cache when the call returns.
 */
bool SF2Cache_PreloadBlock(uint32_t block_index);

/** Request block load into cache. Called by the audio thread on a miss. */
void SF2Cache_RequestBlock(uint32_t block_index);

/** Fetch 16-bit PCM sample for sample_index. Non-blocking audio thread call. */
short SF2Cache_GetSample(tsf *f, uint32_t sample_index, int *last_slot_hint);

/** Get current file offset of the smpl chunk data in SF2 file. */
uint32_t SF2Cache_GetSmplFileOffset(void);

/** Set the smpl chunk data byte offset in the file. */
void SF2Cache_SetSmplFileOffset(uint32_t file_offset, uint32_t total_samples);

/** Copy diagnostic counters. */
void SF2Cache_GetStats(SF2CacheStats *out);

/** Zero diagnostic counters. */
void SF2Cache_ResetStats(void);

#ifdef __cplusplus
}
#endif

#endif /* SF2_CACHE_H */

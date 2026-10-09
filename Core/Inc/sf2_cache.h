/**
 * @file    sf2_cache.h
 * @brief   Bounded RAM PCM sample cache and file backend for TinySoundFont.
 */
#ifndef SF2_CACHE_H
#define SF2_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include "tsf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SF2_BLOCK_SAMPLES 1024U    /* 1024 int16_t samples = 2048 bytes per cache block */
#define SF2_CACHE_BLOCKS  32U      /* 32 blocks x 2 KB = 64 KB total RAM sample cache */

typedef enum {
    SF2_BLOCK_EMPTY   = 0,
    SF2_BLOCK_LOADING = 1,
    SF2_BLOCK_READY   = 2
} SF2BlockState;

/** Open SF2 file using platform file backend (FatFs on firmware, stdio on host). */
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
 * Process block load requests from queue using file backend.
 * Must be called from the main loop context (e.g. main.c).
 */
void SF2Cache_ProcessRequests(void);

/** Request block load into cache. Safe for audio and control threads. */
void SF2Cache_RequestBlock(uint32_t block_index);

/** Fetch 16-bit PCM sample for sample_index. Non-blocking audio thread call. */
short SF2Cache_GetSample(tsf *f, uint32_t sample_index, int *last_slot_hint);

/** Get current file offset of the smpl chunk data in SF2 file. */
uint32_t SF2Cache_GetSmplFileOffset(void);

/** Set the smpl chunk data byte offset in the file. */
void SF2Cache_SetSmplFileOffset(uint32_t file_offset, uint32_t total_samples);

#ifdef __cplusplus
}
#endif

#endif /* SF2_CACHE_H */

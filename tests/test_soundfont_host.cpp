// Automated unit tests for SoundFont synthesizer and SF2 cache.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "sf2_cache.h"
#include "soundfont_synth.h"
#include "synth_engine.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond    \
                      << std::endl;                                              \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static int get_buffer_peak(const int16_t *buf, uint32_t num_frames) {
    int peak = 0;
    uint32_t count = num_frames * 2;
    for (uint32_t i = 0; i < count; ++i) {
        int a = buf[i] < 0 ? -static_cast<int>(buf[i]) : static_cast<int>(buf[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

static void test_engine_registration_and_pc() {
    SynthEngine_Init();
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_EPIANO);

    SynthEngine_ProgramChange(4);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SOUNDFONT);
    CHECK(std::strcmp(SynthEngine_GetName(), "soundfont") == 0);

    SynthEngine_ProgramChange(5);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_EPIANO);

    SynthEngine_ProgramChange(6);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_PLUCK);

    SynthEngine_ProgramChange(9); // 9 % 5 == 4 -> soundfont
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SOUNDFONT);
}

static void test_sf2_loading_and_cache_streaming() {
    // Attempt loading SNDFNT.SF2
    bool ok = SoundFontSynth_InitSF2();
    if (!ok) {
        std::cout << "[INFO] tests/SNDFNT.SF2 not found, skipping streaming audio check" << std::endl;
        return;
    }

    CHECK(SoundFontSynth_IsLoaded() == true);
    CHECK(SF2Cache_IsFileOpen() == true);
    CHECK(SF2Cache_GetSmplFileOffset() > 0);

    // Initial state: empty cache
    SF2Cache_Reset();

    // Trigger note on
    SoundFontSynth_NoteOn(60, 100);

    // Render audio before processing requests -> safe non-blocking return
    std::vector<int16_t> buffer(256 * 2, 0);
    SoundFontSynth_FillStereoBuffer(buffer.data(), 256);

    // Process pending requests in main context
    SoundFontSynth_Process();

    // Render audio after processing requests -> samples loaded into cache
    std::vector<int16_t> buffer2(256 * 2, 0);
    SoundFontSynth_FillStereoBuffer(buffer2.data(), 256);
    CHECK(get_buffer_peak(buffer2.data(), 256) > 0);

    // Send note off
    SoundFontSynth_NoteOff(60);
    SoundFontSynth_FillStereoBuffer(buffer2.data(), 256);
}

static void test_missing_or_corrupt_file_handling() {
    // Voices left over from the previous test are still ringing out (their low-pass filter
    // keeps oscillating for a moment after their sample data disappears). Release them and
    // let them finish, otherwise this test would measure that tail instead of the "no file" case.
    SoundFontSynth_Init();
    SF2Cache_CloseFile();
    {
        std::vector<int16_t> flush(480 * 2, 0);
        for (int i = 0; i < 20; ++i) {
            SoundFontSynth_FillStereoBuffer(flush.data(), 480);
        }
    }

    bool loaded = SF2Cache_OpenFile("non_existent_file.sf2");
    CHECK(loaded == false);
    CHECK(SF2Cache_IsFileOpen() == false);

    // Ensure rendering with missing file yields silence safely without crash
    std::vector<int16_t> buffer(512 * 2, 0x7FFF);
    SoundFontSynth_FillStereoBuffer(buffer.data(), 512);
    CHECK(get_buffer_peak(buffer.data(), 512) == 0);

    // Reset synth engine safely
    SoundFontSynth_Init();
    CHECK(get_buffer_peak(buffer.data(), 512) == 0);
}

static void test_cache_block_requests_and_hits() {
    bool ok = SoundFontSynth_InitSF2();
    if (!ok) {
        return;
    }

    SF2Cache_Reset();

    // Request block 0 and 1
    SF2Cache_RequestBlock(0);
    SF2Cache_RequestBlock(1);

    // Process requests
    SF2Cache_ProcessRequests();
    SF2Cache_ProcessRequests();

    // Trigger NoteOn and fill buffer -> accesses samples via SF2Cache_GetSample
    SoundFontSynth_NoteOn(60, 100);
    std::vector<int16_t> buffer(128 * 2, 0);
    SoundFontSynth_FillStereoBuffer(buffer.data(), 128);

    SoundFontSynth_NoteOff(60);
}

static void test_sf2_cache_reader_protection() {
    bool ok = SoundFontSynth_InitSF2();
    if (!ok) {
        return;
    }

    SF2Cache_Reset();

    // Fill cache blocks 0..31
    for (uint32_t i = 0; i < SF2_CACHE_BLOCKS; ++i) {
        SF2Cache_RequestBlock(i);
        SF2Cache_ProcessRequests();
    }

    // SoundFont synth active note to generate sample requests
    SoundFontSynth_NoteOn(60, 100);

    std::atomic<bool> stop_flag{false};
    std::atomic<bool> thread_started{false};

    // Audio thread context simulator
    std::thread audio_thread([&]() {
        thread_started.store(true, std::memory_order_release);
        std::vector<int16_t> audio_buf(128 * 2, 0);
        while (!stop_flag.load(std::memory_order_relaxed)) {
            SoundFontSynth_FillStereoBuffer(audio_buf.data(), 128);
        }
    });

    while (!thread_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Process background requests and force continuous eviction while audio thread is actively reading.
    // The blocks requested here lie outside the blocks cached above, whatever SF2_CACHE_BLOCKS is.
    const uint32_t passes = 6400U / SF2_CACHE_BLOCKS;
    for (uint32_t pass = 0; pass < passes; ++pass) {
        for (uint32_t b = SF2_CACHE_BLOCKS; b < 2U * SF2_CACHE_BLOCKS; ++b) {
            SF2Cache_RequestBlock(b);
            SF2Cache_ProcessRequests();
        }
    }

    stop_flag.store(true, std::memory_order_release);
    audio_thread.join();

    SoundFontSynth_NoteOff(60);

    // After reader finishes, verify cache can process eviction and load requested blocks properly
    SF2Cache_RequestBlock(100);
    SF2Cache_ProcessRequests();

    std::vector<int16_t> post_buf(128 * 2, 0);
    SoundFontSynth_FillStereoBuffer(post_buf.data(), 128);
}

#include <fstream>

struct TestTSFLayout {
    void *presets;
    float *fontSamples;
    unsigned int smpl_file_offset;
    unsigned int smpl_sample_count;
};

static void test_sf2_cache_load_error_handling() {
    TestTSFLayout fake_tsf_layout;
    std::memset(&fake_tsf_layout, 0, sizeof(fake_tsf_layout));
    tsf *fake_tsf = reinterpret_cast<tsf *>(&fake_tsf_layout);

    // 1. Successful load of full block
    {
        const char *fname = "test_sf2_full.bin";
        std::ofstream ofs(fname, std::ios::binary);
        int16_t samples[1024];
        for (int i = 0; i < 1024; ++i) {
            samples[i] = static_cast<int16_t>(i + 100);
        }
        ofs.write(reinterpret_cast<const char *>(samples), sizeof(samples));
        ofs.close();

        SF2Cache_CloseFile();
        CHECK(SF2Cache_OpenFile(fname) == true);
        SF2Cache_SetSmplFileOffset(0, 1024);
        fake_tsf_layout.smpl_sample_count = 1024;

        SF2Cache_RequestBlock(0);
        SF2Cache_ProcessRequests();

        CHECK(SF2Cache_GetSample(fake_tsf, 0, nullptr) == 100);
        CHECK(SF2Cache_GetSample(fake_tsf, 1023, nullptr) == 1123);

        SF2Cache_CloseFile();
        std::remove(fname);
    }

    // 2. Successful load of last partial block
    {
        const char *fname = "test_sf2_partial.bin";
        std::ofstream ofs(fname, std::ios::binary);
        // Total 1500 samples: block 0 has 1024 samples, block 1 has 476 samples (952 bytes)
        int16_t samples[1500];
        for (int i = 0; i < 1500; ++i) {
            samples[i] = static_cast<int16_t>(i + 500);
        }
        ofs.write(reinterpret_cast<const char *>(samples), sizeof(samples));
        ofs.close();

        SF2Cache_CloseFile();
        CHECK(SF2Cache_OpenFile(fname) == true);
        SF2Cache_SetSmplFileOffset(0, 1500);
        fake_tsf_layout.smpl_sample_count = 1500;

        SF2Cache_RequestBlock(1);
        SF2Cache_ProcessRequests();

        CHECK(SF2Cache_GetSample(fake_tsf, 1024, nullptr) == 1524);
        CHECK(SF2Cache_GetSample(fake_tsf, 1499, nullptr) == 1999);

        SF2Cache_CloseFile();
        std::remove(fname);
    }

    // Note on file seek error testing: Standard C/C++ stdio (fopen/fseek) permits
    // seeking past EOF without error. Thus file boundary and short reads are safely
    // detected and handled by explicit byte count verification after read.

    // 3. Short read error handling, partial data non-publication, & slot reusability
    {
        const char *fname = "test_sf2_short_read.bin";
        std::ofstream ofs(fname, std::ios::binary);
        char dummy[500] = {0}; // Expecting 2048 bytes for 1024 samples
        ofs.write(dummy, sizeof(dummy));
        ofs.close();

        SF2Cache_CloseFile();
        CHECK(SF2Cache_OpenFile(fname) == true);
        SF2Cache_SetSmplFileOffset(0, 1024);
        fake_tsf_layout.smpl_sample_count = 1024;

        SF2Cache_RequestBlock(0);
        SF2Cache_ProcessRequests();

        // Short read must NOT publish partial/garbage data as ready block
        CHECK(SF2Cache_GetSample(fake_tsf, 0, nullptr) == 0);

        // Verify slot remains reusable by loading complete valid data into cache
        std::ofstream ofs_valid(fname, std::ios::binary);
        int16_t valid_samples[1024];
        for (int i = 0; i < 1024; ++i) {
            valid_samples[i] = static_cast<int16_t>(i + 2000);
        }
        ofs_valid.write(reinterpret_cast<const char *>(valid_samples), sizeof(valid_samples));
        ofs_valid.close();

        SF2Cache_ProcessRequests();
        CHECK(SF2Cache_GetSample(fake_tsf, 0, nullptr) == 2000);

        SF2Cache_CloseFile();
        std::remove(fname);
    }

    // 4. Deterministic single-pass queue processing & retry logic
    {
        const char *fname = "test_sf2_retry.bin";
        {
            std::ofstream ofs(fname, std::ios::binary);
            ofs.write("short", 5);
        }

        SF2Cache_CloseFile();
        CHECK(SF2Cache_OpenFile(fname) == true);
        SF2Cache_SetSmplFileOffset(0, 1024);
        fake_tsf_layout.smpl_sample_count = 1024;

        SF2Cache_RequestBlock(0);

        // Single call to ProcessRequests must pop the request, attempt read, fail, re-queue
        // the request, and return immediately without infinite processing loop.
        SF2Cache_ProcessRequests();

        // Verify block 0 is not published
        CHECK(SF2Cache_GetSample(fake_tsf, 0, nullptr) == 0);

        // Re-requesting block 0 while already re-queued should be safely ignored (queue duplicate check)
        SF2Cache_RequestBlock(0);

        // Fix file on disk with complete valid data
        {
            std::ofstream ofs(fname, std::ios::binary);
            int16_t samples[1024];
            for (int i = 0; i < 1024; ++i) {
                samples[i] = static_cast<int16_t>(i + 888);
            }
            ofs.write(reinterpret_cast<const char *>(samples), sizeof(samples));
        }

        // Second ProcessRequests call services the re-queued block 0 request
        SF2Cache_ProcessRequests();

        // Block 0 is now ready with expected sample data
        CHECK(SF2Cache_GetSample(fake_tsf, 0, nullptr) == 888);
        CHECK(SF2Cache_GetSample(fake_tsf, 1023, nullptr) == 1911);

        SF2Cache_CloseFile();
        std::remove(fname);
    }
}


// --- Regression tests for "no sound / noise" in the SoundFont engine ----------------------

// Longest run of consecutive all-zero stereo frames between the first non-zero frame (the
// envelope starts at zero, so the onset is skipped) and to_frame of an interleaved buffer.
static uint32_t longest_zero_run_after_onset(const std::vector<int16_t> &buf, size_t to_frame) {
    size_t i = 0;
    while (i < to_frame && buf[2 * i] == 0 && buf[2 * i + 1] == 0) ++i;
    uint32_t longest = 0, cur = 0;
    for (; i < to_frame; ++i) {
        if (buf[2 * i] == 0 && buf[2 * i + 1] == 0) {
            if (++cur > longest) longest = cur;
        } else {
            cur = 0;
        }
    }
    return longest;
}

// The first blocks of a note are loaded by NoteOn itself, before the voice starts: the very
// first rendered block must already contain the attack and must not hit the cache-miss path.
static void test_noteon_preloads_attack() {
    if (!SoundFontSynth_InitSF2()) return;
    SoundFontSynth_Init();
    SF2Cache_ResetStats();

    SoundFontSynth_NoteOn(60, 100);

    SF2CacheStats st;
    SF2Cache_GetStats(&st);
    CHECK(st.blocks_loaded >= 2);   // C4 is played by two regions (L/R); each preloads its first blocks

    std::vector<int16_t> buf(256 * 2, 0);
    SoundFontSynth_FillStereoBuffer(buf.data(), 256);   // note: no SoundFontSynth_Process() yet
    CHECK(get_buffer_peak(buf.data(), 256) > 0);

    SF2Cache_GetStats(&st);
    CHECK(st.miss_samples == 0);

    SoundFontSynth_NoteOff(60);
    SoundFontSynth_FillStereoBuffer(buf.data(), 256);
}

// The audio callback renders in fixed 10 ms blocks while the main loop services the cache only
// now and then (on Windows sleep_for(1ms) really sleeps ~15.6 ms, i.e. 2 calls per 3 audio
// blocks here). The sound must neither disappear nor be torn by holes of silence.
static void run_streaming_scenario(int note, int velocity, int main_loop_calls_per_3_blocks) {
    if (!SoundFontSynth_InitSF2()) return;
    SoundFontSynth_Init();
    SF2Cache_ResetStats();

    const uint32_t frames = 480;        // 10 ms at 48 kHz
    const int blocks = 80;              // 0.8 s
    const int note_off_block = 60;
    std::vector<int16_t> out((size_t)frames * 2 * blocks, 0);

    SoundFontSynth_NoteOn((uint8_t)note, (uint8_t)velocity);
    for (int b = 0; b < blocks; ++b) {
        if (b == note_off_block) SoundFontSynth_NoteOff((uint8_t)note);
        SoundFontSynth_FillStereoBuffer(&out[(size_t)b * frames * 2], frames);
        if ((b % 3) < main_loop_calls_per_3_blocks) {
            SoundFontSynth_Process();
        }
    }

    // Sound is present at the very beginning and still present near the end of the held note.
    CHECK(get_buffer_peak(&out[0], frames) > 0);
    CHECK(get_buffer_peak(&out[(size_t)(note_off_block - 5) * frames * 2], frames) > 200);

    // No holes of silence while the note sounds (the amplitude is far above 1 LSB there).
    CHECK(longest_zero_run_after_onset(out, (size_t)note_off_block * frames) < 8);

    SF2CacheStats st;
    SF2Cache_GetStats(&st);
    CHECK(st.miss_samples == 0);
    CHECK(st.load_errors == 0);

    SoundFontSynth_NoteOff((uint8_t)note);
}

static void test_streaming_survives_slow_main_loop() {
    run_streaming_scenario(60, 46, 3);   // main loop faster than the audio period
    run_streaming_scenario(60, 46, 2);   // ~15.6 ms main loop (Windows default timer)
    run_streaming_scenario(84, 100, 2);  // high note: samples are consumed faster
    run_streaming_scenario(36, 110, 1);  // ~30 ms main loop
}

// MIDI events must reach TinySoundFont only from the audio context (through the FIFO).
// Hammer the engine from the "main" thread while another thread renders; this is meaningful
// under ThreadSanitizer and must at least never crash or produce out-of-range garbage.
static void test_events_while_rendering() {
    if (!SoundFontSynth_InitSF2()) return;
    SoundFontSynth_Init();

    std::atomic<bool> stop{false};
    std::thread audio([&]() {
        std::vector<int16_t> buf(240 * 2);
        while (!stop.load(std::memory_order_acquire)) {
            SoundFontSynth_FillStereoBuffer(buf.data(), 240);
        }
    });

    for (int i = 0; i < 3000; ++i) {
        uint8_t n = (uint8_t)(36 + (i * 7) % 60);
        SoundFontSynth_NoteOn(n, (uint8_t)(20 + (i * 13) % 100));
        if (i % 3 == 0) SoundFontSynth_ControlChange(64, (uint8_t)((i / 3) % 2 ? 127 : 0));
        SoundFontSynth_Process();
        if (i % 2 == 0) SoundFontSynth_NoteOff(n);
        if (i % 50 == 0) std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    audio.join();

    // Everything still works afterwards.
    SoundFontSynth_Init();
    SoundFontSynth_NoteOn(60, 100);
    std::vector<int16_t> buf(256 * 2, 0);
    SoundFontSynth_FillStereoBuffer(buf.data(), 256);
    CHECK(get_buffer_peak(buf.data(), 256) > 0);
}

// SF2Cache_ProcessRequests() must read ahead of the block the audio thread is playing.
static void test_cache_reads_ahead() {
    if (!SoundFontSynth_InitSF2()) return;
    SF2Cache_Reset();
    SF2Cache_ResetStats();

    TestTSFLayout fake_tsf_layout;
    std::memset(&fake_tsf_layout, 0, sizeof(fake_tsf_layout));
    fake_tsf_layout.smpl_sample_count = 0x0FFFFFFFu;   // only used for the range check in GetSample
    tsf *fake_tsf = reinterpret_cast<tsf *>(&fake_tsf_layout);

    const uint32_t n = SF2_BLOCK_SAMPLES;
    const uint32_t b = 200;
    CHECK(SF2Cache_PreloadBlock(b) == true);

    // The audio thread plays inside block b ...
    (void)SF2Cache_GetSample(fake_tsf, b * n + 10, nullptr);

    // ... and one main-loop pass later the following blocks are resident.
    SF2Cache_ProcessRequests();
    SF2CacheStats st;
    SF2Cache_GetStats(&st);
    CHECK(st.blocks_loaded == 1 + SF2_READAHEAD_BLOCKS);

    SF2Cache_ResetStats();
    for (uint32_t k = 1; k <= SF2_READAHEAD_BLOCKS; ++k) {
        (void)SF2Cache_GetSample(fake_tsf, (b + k) * n + 5, nullptr);
    }
    SF2Cache_GetStats(&st);
    CHECK(st.miss_samples == 0);
}

int main() {
    test_engine_registration_and_pc();
    test_sf2_loading_and_cache_streaming();
    test_missing_or_corrupt_file_handling();
    test_cache_block_requests_and_hits();
    test_sf2_cache_reader_protection();

    test_sf2_cache_load_error_handling();

    test_noteon_preloads_attack();
    test_streaming_survives_slow_main_loop();
    test_events_while_rendering();
    test_cache_reads_ahead();

    if (g_failures != 0) {
        std::cerr << "test_soundfont_host: " << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_soundfont_host passed successfully." << std::endl;
    return 0;
}

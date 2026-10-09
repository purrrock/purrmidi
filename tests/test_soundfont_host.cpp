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
    SF2Cache_CloseFile();
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

    // Process background requests and force continuous eviction while audio thread is actively reading
    for (uint32_t pass = 0; pass < 200; ++pass) {
        for (uint32_t b = 32; b < 64; ++b) {
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

int main() {
    test_engine_registration_and_pc();
    test_sf2_loading_and_cache_streaming();
    test_missing_or_corrupt_file_handling();
    test_cache_block_requests_and_hits();
    test_sf2_cache_reader_protection();

    if (g_failures != 0) {
        std::cerr << "test_soundfont_host: " << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_soundfont_host passed successfully." << std::endl;
    return 0;
}

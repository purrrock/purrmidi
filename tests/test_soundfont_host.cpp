// Автотесты синтезатора SoundFont: банк читается на месте (хост-аналог QSPI XIP), см. Platform/host.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include <cstdio>
#include <string>

#include "sf2_loader_host.h"
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
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_DULCIMER);

    SynthEngine_ProgramChange(6);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_FLUTE);

    SynthEngine_ProgramChange(7);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SAXOPHONE);

    SynthEngine_ProgramChange(8); // 8 % 8 == 0 -> epiano
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_EPIANO);

    SynthEngine_ProgramChange(9); // 9 % 8 == 1 -> pluck
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_PLUCK);

    SynthEngine_ProgramChange(12); // 12 % 8 == 4 -> soundfont
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SOUNDFONT);
}

static void test_bank_loading_and_rendering() {
    SF2LoaderHost_SetPath(nullptr);   // поиск банка по умолчанию
    if (!SoundFontSynth_InitSF2()) {
        std::cout << "[INFO] SoundFont bank not found (tests/SNDFNT.SF2), skipping rendering checks" << std::endl;
        return;
    }

    CHECK(SoundFontSynth_IsLoaded() == true);
    CHECK(SoundFontSynth_GetSampleCount() > 0);

    SoundFontSynth_NoteOn(60, 100);

    // Образцы читаются прямо из банка: звук есть уже в первом блоке, без фоновой подкачки.
    std::vector<int16_t> buffer(256 * 2, 0);
    SoundFontSynth_FillStereoBuffer(buffer.data(), 256);
    CHECK(get_buffer_peak(buffer.data(), 256) > 0);

    SoundFontSynth_NoteOff(60);
    SoundFontSynth_FillStereoBuffer(buffer.data(), 256);
}

static std::string write_temp_file(const char *name, const std::vector<uint8_t> &data) {
    std::string path = std::string("test_sf2_tmp_") + name + ".sf2";
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
    return path;
}

static void test_missing_or_corrupt_bank_handling() {
    // Остатки голосов от предыдущего теста не должны влиять на проверку «нет банка».
    SoundFontSynth_Init();
    {
        std::vector<int16_t> flush(480 * 2, 0);
        for (int i = 0; i < 20; ++i) SoundFontSynth_FillStereoBuffer(flush.data(), 480);
    }

    // 1. Файла нет: загрузка завершается ошибкой, синтезатор безопасно молчит.
    SF2LoaderHost_SetPath("non_existent_file.sf2");
    CHECK(SoundFontSynth_InitSF2() == false);
    CHECK(SoundFontSynth_IsLoaded() == false);
    CHECK(SoundFontSynth_IsAvailable() == false);
    std::vector<int16_t> buffer(512 * 2, 0x7FFF);
    SoundFontSynth_NoteOn(60, 100);                        // игнорируется: банка нет
    SoundFontSynth_FillStereoBuffer(buffer.data(), 512);
    CHECK(get_buffer_peak(buffer.data(), 512) == 0);

    // 2. Мусор вместо банка: tsf_load() отвергает его.
    {
        std::vector<uint8_t> junk(4096);
        for (size_t i = 0; i < junk.size(); ++i) junk[i] = (uint8_t)(i * 31 + 7);
        const std::string path = write_temp_file("junk", junk);
        SF2LoaderHost_SetPath(path.c_str());
        CHECK(SoundFontSynth_InitSF2() == false);
        CHECK(SoundFontSynth_IsLoaded() == false);
        SoundFontSynth_FillStereoBuffer(buffer.data(), 512);
        CHECK(get_buffer_peak(buffer.data(), 512) == 0);
        std::remove(path.c_str());
    }

    // 3. Обрезанный банк (оборван посреди данных): без выхода за границы памяти, звук либо тишина, либо в диапазоне.
    {
        SF2LoaderHost_SetPath(nullptr);
        SF2LoaderHost_Unload();
        const uint8_t *base = nullptr;
        uint32_t size = 0;
        if (SF2Loader_Ensure() && SF2Loader_GetBank(&base, &size) && size > 200000) {
            std::vector<uint8_t> cut(base, base + 200000);
            const std::string path = write_temp_file("cut", cut);
            SF2LoaderHost_SetPath(path.c_str());
            const bool ok = SoundFontSynth_InitSF2();
            if (ok) {
                SoundFontSynth_NoteOn(60, 127);
                for (int i = 0; i < 20; ++i) SoundFontSynth_FillStereoBuffer(buffer.data(), 512);
            }
            std::remove(path.c_str());
        }
    }

    // Возврат к банку по умолчанию: движок снова работоспособен.
    SF2LoaderHost_SetPath(nullptr);
    SoundFontSynth_Init();
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

// Удерживаемая нота не должна «рваться» дырками тишины и пропадать (регрессия «нет звука / шум»).
static void run_sustain_scenario(int note, int velocity) {
    if (!SoundFontSynth_InitSF2()) return;
    SoundFontSynth_Init();

    const uint32_t frames = 480;        // 10 ms at 48 kHz
    const int blocks = 80;              // 0.8 s
    const int note_off_block = 60;
    std::vector<int16_t> out((size_t)frames * 2 * blocks, 0);

    SoundFontSynth_NoteOn((uint8_t)note, (uint8_t)velocity);
    for (int b = 0; b < blocks; ++b) {
        if (b == note_off_block) SoundFontSynth_NoteOff((uint8_t)note);
        SoundFontSynth_FillStereoBuffer(&out[(size_t)b * frames * 2], frames);
    }

    // Sound is present at the very beginning and still present near the end of the held note.
    CHECK(get_buffer_peak(&out[0], frames) > 0);
    CHECK(get_buffer_peak(&out[(size_t)(note_off_block - 5) * frames * 2], frames) > 200);

    // No holes of silence while the note sounds (the amplitude is far above 1 LSB there).
    CHECK(longest_zero_run_after_onset(out, (size_t)note_off_block * frames) < 8);

    SoundFontSynth_NoteOff((uint8_t)note);
}

static void test_sustained_notes_have_no_holes() {
    run_sustain_scenario(60, 46);
    run_sustain_scenario(84, 100);   // высокая нота: сэмплы читаются быстрее
    run_sustain_scenario(36, 110);
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

int main() {
    test_engine_registration_and_pc();
    test_bank_loading_and_rendering();
    test_missing_or_corrupt_bank_handling();
    test_sustained_notes_have_no_holes();
    test_events_while_rendering();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_soundfont_host passed successfully." << std::endl;
    return 0;
}

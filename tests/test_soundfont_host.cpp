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

#include <algorithm>
#include <cstdio>
#include <string>

#include "sf2_loader_host.h"
#include "soundfont_synth.h"
#include "synth_engine.h"
#include "check.h"

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
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_VIOLIN);

    SynthEngine_ProgramChange(5);   // SoundFont - в конце списка
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SOUNDFONT);
    CHECK(std::strcmp(SynthEngine_GetName(), "soundfont") == 0);

    const uint32_t total = SynthEngine_GetProgramCount();   // 5 встроенных + пресеты загруженного банка
    CHECK(total >= 6U);
    SynthEngine_ProgramChange((uint8_t)(total % 128U == 0U ? 0U : total));   // total % total == 0 -> epiano
    if (total <= 126U) CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_EPIANO);

    SynthEngine_ProgramChange(1);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_PLUCK);
}

// Все пресеты банка - отдельные программы: имена "SF:<имя>", переключение пресета без сброса движка.
static void test_presets_are_programs() {
    SF2LoaderHost_SetPath(nullptr);
    SynthEngine_Init();
    if (!SoundFontSynth_InitSF2()) {
        std::cout << "[INFO] SoundFont bank not found, skipping preset list test" << std::endl;
        return;
    }
    const uint32_t presets = SoundFontSynth_GetPresetCount();
    CHECK(presets >= 1U);
    CHECK(SynthEngine_GetProgramCount() == (uint32_t)SYNTH_ENGINE_SOUNDFONT + presets);
    CHECK(SoundFontSynth_GetPresetName(presets) == nullptr);   // за пределами списка

    char name[18];
    SynthEngine_GetProgramName(0, name, sizeof(name));
    CHECK(std::strcmp(name, "E-Piano") == 0);
    SynthEngine_GetProgramName(SYNTH_ENGINE_VIOLIN, name, sizeof(name));
    CHECK(std::strcmp(name, "Violin") == 0);
    for (uint32_t k = 0; k < presets && k < 5U; ++k) {
        SynthEngine_GetProgramName((uint32_t)SYNTH_ENGINE_SOUNDFONT + k, name, sizeof(name));
        CHECK(std::strncmp(name, "SF:", 3) == 0);
        CHECK(std::strlen(name) <= 17U);
        CHECK(name[std::strlen(name) - 1U] != ' ');            // хвостовые пробелы срезаны
    }

    // Выбор последнего пресета: движок остаётся SoundFont, номер программы сохраняется, звук есть.
    const uint32_t last = (uint32_t)SYNTH_ENGINE_SOUNDFONT + presets - 1U;
    CHECK(SynthEngine_SelectProgram(last) == 1);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SOUNDFONT);
    CHECK(SynthEngine_GetProgram() == last);
    CHECK(SoundFontSynth_GetPreset() == presets - 1U);
    CHECK(SynthEngine_SelectProgram(last + 1U) == 0);          // вне списка
    CHECK(SynthEngine_GetProgram() == last);

    SynthEngine_NoteOn(60, 100);
    std::vector<int16_t> buffer(256 * 2, 0);
    SynthEngine_FillStereoBuffer(buffer.data(), 256);
    CHECK(get_buffer_peak(buffer.data(), 256) > 0);
    SynthEngine_NoteOff(60);

    // Panic пересоздаёт движок, но выбранный пресет сохраняется.
    SynthEngine_Panic();
    CHECK(SynthEngine_GetProgram() == last);

    // Program Change 127 обходит список по кругу и доходит до пресетов банка.
    SynthEngine_SelectProgram(SYNTH_ENGINE_VIOLIN);
    SynthEngine_SelectNext();
    CHECK(SynthEngine_GetProgram() == (uint32_t)SYNTH_ENGINE_SOUNDFONT);
    if (presets > 1U) {
        SynthEngine_SelectNext();
        CHECK(SynthEngine_GetProgram() == (uint32_t)SYNTH_ENGINE_SOUNDFONT + 1U);
    }
    SynthEngine_Init();
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

// Контракт XIP: пока окно банка закрыто (на плате - QSPI в индексном режиме), синтезатор не читает банк и молчит;
// когда окно снова открыто - звук продолжается. Ни ноты, ни рендер не должны падать.
static void test_xip_window_closed_gives_silence_and_recovers() {
    SF2LoaderHost_SetPath(nullptr);
    if (!SoundFontSynth_InitSF2()) {
        std::cout << "[INFO] SoundFont bank not found, skipping XIP guard test" << std::endl;
        return;
    }
    std::vector<int16_t> buffer(256 * 2, 0x5555);

    SoundFontSynth_NoteOn(60, 110);
    SoundFontSynth_FillStereoBuffer(buffer.data(), 256);
    CHECK(get_buffer_peak(buffer.data(), 256) > 0);        // окно открыто: звук есть

    SF2LoaderHost_SetReadable(false);                      // «QSPI вышел из XIP»
    std::fill(buffer.begin(), buffer.end(), (int16_t)0x5555);
    SoundFontSynth_NoteOn(64, 110);                        // события в этот момент не теряются и не ломают рендер
    SoundFontSynth_FillStereoBuffer(buffer.data(), 256);
    CHECK(get_buffer_peak(buffer.data(), 256) == 0);       // буфер перезаписан тишиной, банк не читался

    SF2LoaderHost_SetReadable(true);                       // окно снова открыто
    SoundFontSynth_FillStereoBuffer(buffer.data(), 256);
    CHECK(get_buffer_peak(buffer.data(), 256) > 0);        // звук вернулся
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

// Note Off не должен теряться при переполнении FIFO (раньше ноты оставались звучать без нажатых клавиш).
static void test_note_off_survives_fifo_overflow() {
    SF2LoaderHost_SetPath(nullptr);
    if (!SoundFontSynth_InitSF2()) {
        std::cout << "[INFO] SoundFont bank not found, skipping overflow test" << std::endl;
        return;
    }
    std::vector<int16_t> buffer(256 * 2, 0);
    for (int i = 0; i < 600; ++i) SoundFontSynth_NoteOn((uint8_t)(30 + (i % 60)), 100);
    for (int i = 0; i < 600; ++i) SoundFontSynth_NoteOff((uint8_t)(30 + (i % 60)));   // без рендера между
    for (int i = 0; i < 100; ++i) SoundFontSynth_FillStereoBuffer(buffer.data(), 256); // ~0.5 с
    SoundFontSynth_FillStereoBuffer(buffer.data(), 256);
    CHECK(get_buffer_peak(buffer.data(), 256) == 0);       // тишина: ничего не залипло

    SoundFontSynth_NoteOn(60, 100);                        // после восстановления звук работает
    SoundFontSynth_FillStereoBuffer(buffer.data(), 256);
    CHECK(get_buffer_peak(buffer.data(), 256) > 0);
}

// Колесо высоты тона реально сдвигает частоту (на ±2 полутона), а возврат в центр возвращает исходную.
static double sf_estimate_f0(int bend, double f_expected) {
    SoundFontSynth_Init();                                 // чистые голоса и колесо в центре
    SoundFontSynth_PitchBend((int16_t)bend);
    SoundFontSynth_NoteOn(57, 100);                        // A3 = 220 Гц
    std::vector<int16_t> buf(256 * 2, 0);
    std::vector<double> x;
    for (int blk = 0; blk < 40; ++blk) {
        SoundFontSynth_FillStereoBuffer(buf.data(), 256);
        if (blk < 20) continue;                            // пропускаем атаку и выравнивание
        for (int i = 0; i < 256; ++i) x.push_back(buf[i * 2]);
    }
    const size_t win = 4096;
    const int lo = (int)(48000.0 / (f_expected * 1.3));
    const int hi = (int)(48000.0 / (f_expected * 0.7));
    std::vector<double> r(hi + 2, 0.0);
    for (int lag = lo - 1; lag <= hi + 1; ++lag) {
        double acc = 0;
        for (size_t i = 0; i < win; ++i) acc += x[i] * x[i + lag];
        r[lag] = acc;
    }
    int best = lo;
    for (int lag = lo; lag <= hi; ++lag) if (r[lag] > r[best]) best = lag;
    const double d = r[best - 1] - 2 * r[best] + r[best + 1];
    const double shift = d != 0 ? 0.5 * (r[best - 1] - r[best + 1]) / d : 0.0;
    return 48000.0 / (best + shift);
}

static void test_pitch_bend_changes_sound() {
    SF2LoaderHost_SetPath(nullptr);
    if (!SoundFontSynth_InitSF2()) {
        std::cout << "[INFO] SoundFont bank not found, skipping pitch bend test" << std::endl;
        return;
    }
    const double center = sf_estimate_f0(0, 220.0);
    const double up     = sf_estimate_f0(8191, 247.0);
    const double down   = sf_estimate_f0(-8192, 196.0);
    const double back   = sf_estimate_f0(0, 220.0);
    std::cout << "sf pitch bend: centre=" << center << " up=" << up << " down=" << down << " back=" << back << std::endl;
    CHECK(std::fabs(center - 220.0) < 4.0);
    CHECK(std::fabs(up   - 220.0 * std::pow(2.0,  2.0 / 12.0)) < 5.0);
    CHECK(std::fabs(down - 220.0 * std::pow(2.0, -2.0 / 12.0)) < 5.0);
    CHECK(std::fabs(back - 220.0) < 4.0);
}

int main() {
    test_engine_registration_and_pc();
    test_presets_are_programs();
    test_bank_loading_and_rendering();
    test_xip_window_closed_gives_silence_and_recovers();
    test_missing_or_corrupt_bank_handling();
    test_sustained_notes_have_no_holes();
    test_events_while_rendering();
    test_note_off_survives_fifo_overflow();
    test_pitch_bend_changes_sound();

    if (purrmidi_test::Failures() != 0) {
        std::cerr << purrmidi_test::Failures() << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_soundfont_host passed successfully." << std::endl;
    return 0;
}

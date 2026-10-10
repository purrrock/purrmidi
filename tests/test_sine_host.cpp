#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include "sine_synth.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); std::abort(); } } while (0)

static std::vector<int16_t> render(uint32_t frames) {
    std::vector<int16_t> buf(frames * 2, 0);
    SineSynth_FillStereoBuffer(buf.data(), frames);
    return buf;
}

// Частота по числу переходов через ноль снизу вверх на отрезке [from, to) кадров
static double zero_cross_freq(const std::vector<int16_t>& b, size_t from, size_t to) {
    int crossings = 0;
    for (size_t i = from + 1; i < to; ++i) {
        if (b[(i - 1) * 2] < 0 && b[i * 2] >= 0) crossings++;
    }
    return crossings / ((to - from) / 48000.0);
}

static bool all_zero(const std::vector<int16_t>& b, size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        if (b[i * 2] != 0 || b[i * 2 + 1] != 0) return false;
    }
    return true;
}

static void test_pitch_and_stereo() {
    SineSynth_Init();
    CHECK(all_zero(render(480), 0, 480));          // до NoteOn — тишина

    SineSynth_NoteOn(69, 100);                      // A4 = 440 Гц
    auto b = render(48000);
    double f = zero_cross_freq(b, 4800, 48000);
    std::cout << "sine A4: " << f << " Hz\n";
    CHECK(std::fabs(f - 440.0) < 2.0);

    int peak = 0;
    for (size_t i = 0; i < 48000; ++i) {
        CHECK(b[i * 2] == b[i * 2 + 1]);
        peak = std::max(peak, std::abs((int)b[i * 2]));
    }
    CHECK(peak > 3000);

    SineSynth_NoteOn(57, 100);                      // A3 = 220 Гц (последняя нота)
    b = render(48000);
    f = zero_cross_freq(b, 4800, 48000);
    CHECK(std::fabs(f - 220.0) < 2.0);
}

static void test_release_and_sustain() {
    SineSynth_Init();
    SineSynth_NoteOn(60, 100);
    render(4800);

    SineSynth_NoteOff(61);                          // чужая нота — игнорируется
    auto b = render(4800);
    CHECK(!all_zero(b, 0, 4800));

    SineSynth_NoteOff(60);
    b = render(4800);                               // 100 мс > релиз 50 мс
    CHECK(all_zero(b, 3000, 4800));

    // Велосити 0 == NoteOff
    SineSynth_NoteOn(60, 100);
    render(2400);
    SineSynth_NoteOn(60, 0);
    CHECK(all_zero(render(4800), 3000, 4800));

    // Педаль сустейна удерживает звук после NoteOff
    SineSynth_ControlChange(64, 127);
    SineSynth_NoteOn(60, 100);
    render(2400);
    SineSynth_NoteOff(60);
    CHECK(!all_zero(render(24000), 20000, 24000));
    SineSynth_ControlChange(64, 0);
    CHECK(all_zero(render(4800), 3000, 4800));

    // CC7 = 0 -> тишина
    SineSynth_NoteOn(60, 100);
    SineSynth_ControlChange(7, 0);
    CHECK(all_zero(render(4800), 0, 4800));
}

static void test_cc120_cc123_and_reinit_regressions() {
    // 1. CC 120 (All Sound Off) — сбрасывает gate/sustain и затухает через релиз до тишины
    SineSynth_Init();
    SineSynth_ControlChange(64, 127);
    SineSynth_NoteOn(60, 100);
    render(2400);
    SineSynth_ControlChange(120, 0);
    auto cc120_buf = render(4800);
    CHECK(all_zero(cc120_buf, 3000, 4800));

    // 2. CC 123 (All Notes Off) — отпускает ноту и сбрасывает сустейн
    SineSynth_Init();
    SineSynth_ControlChange(64, 127); // с зажатым сустейном
    SineSynth_NoteOn(60, 100);
    render(2400);
    SineSynth_ControlChange(123, 0);
    auto release_buf = render(4800);
    CHECK(all_zero(release_buf, 3000, 4800));

    // 3. Re-initialization (SineSynth_Init) — сбрасывает состояние и восстанавливает дефолтный громкость CC7
    SineSynth_Init();
    SineSynth_ControlChange(7, 0); // mute via CC7
    SineSynth_NoteOn(60, 100);
    CHECK(all_zero(render(2400), 0, 2400)); // quiet due to CC7=0

    SineSynth_Init(); // Reinit restores state
    SineSynth_NoteOn(60, 100);
    auto reinit_buf = render(2400);
    CHECK(!all_zero(reinit_buf, 0, 2400)); // sound restored
}

int main() {
    test_pitch_and_stereo();
    test_release_and_sustain();
    test_cc120_cc123_and_reinit_regressions();
    std::cout << "test_sine_host passed successfully." << std::endl;
    return 0;
}

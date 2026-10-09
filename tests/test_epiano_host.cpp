#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>
#include "epiano_synth.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); std::abort(); } } while (0)

static const uint32_t SR = 48000;

static std::vector<int16_t> render(uint32_t frames) {
    std::vector<int16_t> buf((size_t)frames * 2, 0);
    EPianoSynth_FillStereoBuffer(buf.data(), frames);
    return buf;
}

// Основная частота по автокорреляции левого канала (лаги в диапазоне периодов f0 +-30 %)
static double estimate_f0(const std::vector<int16_t>& b, size_t start, size_t window, double f_expected) {
    size_t lag_min = (size_t)(SR / (f_expected * 1.3));
    size_t lag_max = (size_t)(SR / (f_expected * 0.7));
    double best_r = -1e30;
    size_t best_lag = lag_min;
    for (size_t lag = lag_min; lag <= lag_max; ++lag) {
        double r = 0.0;
        for (size_t i = 0; i < window; ++i) {
            r += (double)b[(start + i) * 2] * (double)b[(start + i + lag) * 2];
        }
        if (r > best_r) { best_r = r; best_lag = lag; }
    }
    // параболическая интерполяция пика для точности
    auto corr = [&](size_t lag) {
        double r = 0.0;
        for (size_t i = 0; i < window; ++i) r += (double)b[(start + i) * 2] * (double)b[(start + i + lag) * 2];
        return r;
    };
    double y0 = corr(best_lag - 1), y1 = best_r, y2 = corr(best_lag + 1);
    double denom = (y0 - 2 * y1 + y2);
    double shift = denom != 0.0 ? 0.5 * (y0 - y2) / denom : 0.0;
    return SR / ((double)best_lag + shift);
}

static int peak_of(const std::vector<int16_t>& b, size_t from, size_t to) {
    int p = 0;
    for (size_t i = from * 2; i < to * 2; ++i) p = std::max(p, std::abs((int)b[i]));
    return p;
}

static double rms_of(const std::vector<int16_t>& b, size_t from, size_t to, int ch = -1) {
    double acc = 0.0; size_t n = 0;
    for (size_t i = from; i < to; ++i) {
        for (int c = 0; c < 2; ++c) {
            if (ch >= 0 && c != ch) continue;
            double s = b[i * 2 + c]; acc += s * s; n++;
        }
    }
    return std::sqrt(acc / (double)n);
}

static bool all_zero(const std::vector<int16_t>& b, size_t from, size_t to) {
    for (size_t i = from * 2; i < to * 2; ++i) if (b[i] != 0) return false;
    return true;
}

// Доля энергии в высоких частотах: энергия первой разности / энергия сигнала
static double brightness_ratio(const std::vector<int16_t>& b, size_t from, size_t to) {
    double e = 0.0, d = 0.0;
    for (size_t i = from + 1; i < to; ++i) {
        double s = b[i * 2], p = b[(i - 1) * 2];
        e += s * s; d += (s - p) * (s - p);
    }
    return e > 0.0 ? d / e : 0.0;
}

static void test_pitch_accuracy() {
    const uint8_t notes[] = {36, 48, 60, 69, 72, 81};
    for (uint8_t n : notes) {
        EPianoSynth_Init();
        EPianoSynth_NoteOn(n, 100);
        auto b = render(SR / 2);
        double expected = 440.0 * std::pow(2.0, (n - 69) / 12.0);
        double f = estimate_f0(b, 4800, 4096, expected);
        std::cout << "epiano note " << (int)n << ": " << f << " Hz (expected " << expected << ")\n";
        CHECK(std::fabs(f - expected) / expected < 0.01);
        CHECK(peak_of(b, 0, SR / 2) > 1500);
    }
}

static void test_dynamics_and_headroom() {
    EPianoSynth_Init();
    EPianoSynth_NoteOn(60, 25);
    auto soft = render(SR / 2);
    EPianoSynth_Init();
    EPianoSynth_NoteOn(60, 120);
    auto loud = render(SR / 2);

    double r_soft = rms_of(soft, 2400, SR / 2), r_loud = rms_of(loud, 2400, SR / 2);
    std::cout << "rms soft=" << r_soft << " loud=" << r_loud << "\n";
    CHECK(r_loud > 2.5 * r_soft);
    CHECK(r_soft > 50.0);

    // Тембр: сильный удар ярче слабого (в начале ноты)
    CHECK(brightness_ratio(loud, 200, 6000) > brightness_ratio(soft, 200, 6000));

    // Одна нота ff не доходит до клиппинга
    int p = peak_of(loud, 0, SR / 2);
    std::cout << "peak single ff = " << p << "\n";
    CHECK(p < 28000);

    // Плотный аккорд ff: лимитер держит в пределах int16 без переполнения
    EPianoSynth_Init();
    for (int n = 48; n < 56; ++n) EPianoSynth_NoteOn((uint8_t)n, 127);
    auto chord = render(SR);
    int pc = peak_of(chord, 0, SR);
    std::cout << "peak 8-note ff chord = " << pc << "\n";
    CHECK(pc <= 32767);
    CHECK(pc > 5000);
    // нет скачков, характерных для wrap-around переполнения
    for (size_t i = 1; i < SR; ++i) {
        CHECK(std::abs((int)chord[i * 2] - (int)chord[(i - 1) * 2]) < 20000);
    }
}

static void test_release_and_sustain_pedal() {
    // Релиз: после NoteOff голос затухает до точного нуля
    EPianoSynth_Init();
    EPianoSynth_NoteOn(64, 100);
    render(SR / 4);
    EPianoSynth_NoteOff(64);
    auto tail = render(SR * 3);
    CHECK(all_zero(tail, (size_t)(SR * 2.8), SR * 3));
    CHECK(EPianoSynth_GetActiveVoiceCount() == 0);

    // NoteOn с velocity 0 == NoteOff
    EPianoSynth_Init();
    EPianoSynth_NoteOn(64, 100);
    render(SR / 4);
    EPianoSynth_NoteOn(64, 0);
    tail = render(SR * 3);
    CHECK(all_zero(tail, (size_t)(SR * 2.8), SR * 3));

    // Педаль сустейна: звук живёт после NoteOff, после отпускания педали — затихает
    EPianoSynth_Init();
    EPianoSynth_ControlChange(64, 127);
    EPianoSynth_NoteOn(64, 100);
    render(SR / 4);
    EPianoSynth_NoteOff(64);
    auto held = render(SR);
    CHECK(rms_of(held, (size_t)(SR * 0.8), SR) > 100.0);
    EPianoSynth_ControlChange(64, 0);
    tail = render(SR * 3);
    CHECK(all_zero(tail, (size_t)(SR * 2.8), SR * 3));

    // Удерживаемая нота спадает (электропиано не органно): к 4 с тише, чем в начале
    EPianoSynth_Init();
    EPianoSynth_NoteOn(72, 100);
    auto d = render(SR * 5);
    double r0 = rms_of(d, 2400, 12000), r1 = rms_of(d, SR * 4, SR * 4 + 9600);
    std::cout << "held decay: rms early=" << r0 << " late=" << r1 << "\n";
    CHECK(r1 < 0.6 * r0);
}

static void test_polyphony_and_voice_management() {
    EPianoSynth_Init();
    for (int n = 48; n < 60; ++n) EPianoSynth_NoteOn((uint8_t)n, 100);   // 12 нот
    auto b = render(SR / 4);
    CHECK(EPianoSynth_GetActiveVoiceCount() <= 8);
    CHECK(EPianoSynth_GetActiveVoiceCount() >= 8);
    CHECK(peak_of(b, 0, SR / 4) > 1000);

    // Повторное нажатие той же ноты не занимает второй голос
    EPianoSynth_Init();
    EPianoSynth_NoteOn(60, 100);
    render(2400);
    EPianoSynth_NoteOn(60, 100);
    render(2400);
    CHECK(EPianoSynth_GetActiveVoiceCount() == 1);

    // Две разные ноты — два голоса
    EPianoSynth_NoteOn(64, 100);
    render(2400);
    CHECK(EPianoSynth_GetActiveVoiceCount() == 2);

    // All Sound Off гасит всё быстро, даже при зажатой педали
    EPianoSynth_ControlChange(64, 127);
    EPianoSynth_ControlChange(120, 0);
    auto k = render(SR / 4);
    CHECK(all_zero(k, SR / 10, SR / 4));
    CHECK(EPianoSynth_GetActiveVoiceCount() == 0);

    // All Notes Off отпускает ноты (они затухают по релизу)
    EPianoSynth_Init();
    EPianoSynth_NoteOn(60, 100);
    EPianoSynth_NoteOn(67, 100);
    render(2400);
    EPianoSynth_ControlChange(123, 0);
    auto t = render(SR * 3);
    CHECK(all_zero(t, (size_t)(SR * 2.8), SR * 3));
}

static void test_block_size_independence() {
    // Результат не должен зависеть от того, как поток разбит на блоки (DMA half/full)
    auto run = [](uint32_t block) {
        EPianoSynth_Init();
        EPianoSynth_NoteOn(60, 90);
        EPianoSynth_NoteOn(64, 70);
        EPianoSynth_NoteOn(67, 110);
        std::vector<int16_t> out((size_t)SR * 2);
        uint32_t done = 0;
        while (done < SR) {
            uint32_t n = std::min(block, SR - done);
            EPianoSynth_FillStereoBuffer(&out[(size_t)done * 2], n);
            done += n;
        }
        return out;
    };
    auto a = run(SR);
    auto b = run(128);
    auto c = run(37);
    CHECK(a == b);
    CHECK(a == c);
}

static void test_stereo_spread_and_brightness() {
    // Низкая нота смещена влево, высокая — вправо
    EPianoSynth_Init();
    EPianoSynth_NoteOn(36, 100);
    auto lo = render(SR / 2);
    CHECK(rms_of(lo, 2400, SR / 2, 0) > rms_of(lo, 2400, SR / 2, 1));
    EPianoSynth_Init();
    EPianoSynth_NoteOn(84, 100);
    auto hi = render(SR / 2);
    CHECK(rms_of(hi, 2400, SR / 2, 1) > rms_of(hi, 2400, SR / 2, 0));

    // CC74: больше яркость -> больше высоких гармоник
    EPianoSynth_Init();
    EPianoSynth_ControlChange(74, 5);
    EPianoSynth_NoteOn(60, 100);
    auto dark = render(SR / 2);
    EPianoSynth_Init();
    EPianoSynth_ControlChange(74, 127);
    EPianoSynth_NoteOn(60, 100);
    auto bright = render(SR / 2);
    double rd = brightness_ratio(dark, 200, 12000), rb = brightness_ratio(bright, 200, 12000);
    std::cout << "brightness dark=" << rd << " bright=" << rb << "\n";
    CHECK(rb > 1.3 * rd);

    // CC7 = 0 -> тишина
    EPianoSynth_Init();
    EPianoSynth_ControlChange(7, 0);
    EPianoSynth_NoteOn(60, 100);
    CHECK(all_zero(render(SR / 4), 0, SR / 4));
}

static void test_event_fifo_overflow() {
    EPianoSynth_Init();
    for (int i = 0; i < 300; ++i) EPianoSynth_NoteOn((uint8_t)(40 + (i % 40)), 100);
    CHECK(EPianoSynth_GetDroppedEventCount() > 0);
    auto b = render(SR / 10);                       // не падает, звук есть
    CHECK(peak_of(b, 0, SR / 10) > 0);
    EPianoSynth_NoteOn(200, 100);                   // некорректная нота игнорируется
    EPianoSynth_ControlChange(1, 200);              // значение зажимается
    render(256);
}

int main() {
    test_pitch_accuracy();
    test_dynamics_and_headroom();
    test_release_and_sustain_pedal();
    test_polyphony_and_voice_management();
    test_block_size_independence();
    test_stereo_spread_and_brightness();
    test_event_fifo_overflow();
    std::cout << "test_epiano_host passed successfully." << std::endl;
    return 0;
}

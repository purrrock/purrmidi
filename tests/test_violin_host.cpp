// Тесты синтезатора скрипки (Engine/synth/violin_synth.*): звук, строй, MIDI-управление, полифония, вытеснение голосов.
#include "violin_synth.h"
#include "synth_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#define PURRMIDI_CHECK_FAIL_FAST   // линейный тест: после первой ошибки продолжать бессмысленно
#include "check.h"

namespace {

constexpr uint32_t SR = 48000;

std::vector<int16_t> render(uint32_t frames)
{
    std::vector<int16_t> out((size_t)frames * 2);
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min<uint32_t>(frames - done, 128);
        ViolinSynth_FillStereoBuffer(out.data() + (size_t)done * 2, n);
        done += n;
    }
    return out;
}

int peak_of(const std::vector<int16_t> &b)
{
    int p = 0;
    for (int16_t s : b) p = std::max(p, std::abs((int)s));
    return p;
}

double rms_of(const std::vector<int16_t> &b)
{
    double e = 0;
    for (int16_t s : b) e += (double)s * s;
    return std::sqrt(e / (double)b.size()) / 32768.0;
}

// Моно (L) из стерео-буфера.
std::vector<double> mono(const std::vector<int16_t> &b, size_t first = 0, size_t count = (size_t)-1)
{
    const size_t frames = b.size() / 2;
    const size_t end = (count == (size_t)-1) ? frames : std::min(frames, first + count);
    std::vector<double> x;
    for (size_t i = first; i < end; ++i) x.push_back(b[i * 2]);
    return x;
}

// Основная частота по нормированной автокорреляции с параболической интерполяцией пика.
double f0_of(const std::vector<double> &x, double f_expected)
{
    const int lo = (int)(SR / (f_expected * 1.25));
    const int hi = (int)(SR / (f_expected * 0.8));
    std::vector<double> r(hi + 2, 0.0);
    int best = lo;
    for (int l = lo - 1; l <= hi + 1; ++l) {
        double s = 0, e1 = 0, e2 = 0;
        for (size_t i = 0; i + l < x.size(); ++i) { s += x[i] * x[i + l]; e1 += x[i] * x[i]; e2 += x[i + l] * x[i + l]; }
        r[l] = s / std::sqrt(e1 * e2 + 1e-30);
        if (l >= lo && l <= hi && r[l] > r[best]) best = l;
    }
    const double a = r[best - 1], b = r[best], c = r[best + 1];
    const double d = (a - c) / (2.0 * (a - 2.0 * b + c));
    return SR / (best + d);
}

double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }
double midi_hz(int n) { return 440.0 * std::pow(2.0, (n - 69) / 12.0); }

// Доля энергии «высоких частот»: энергия первой разности к энергии сигнала (растёт с яркостью).
double brightness_of(const std::vector<double> &x)
{
    double e = 0, d = 0;
    for (size_t i = 1; i < x.size(); ++i) { e += x[i] * x[i]; d += (x[i] - x[i - 1]) * (x[i] - x[i - 1]); }
    return d / (e + 1e-30);
}

void fresh()
{
    SynthEngine_Init();                 // сбрасывает арену на E-Piano
    ViolinSynth_Init();                 // арена снова у скрипки
}

}  // namespace

int main()
{
    std::puts("Running ViolinSynth host tests...");

    // 1. После Init - тишина.
    {
        fresh();
        CHECK(peak_of(render(1024)) == 0);
    }

    // 2. Note On звучит; Note Off - затухает до полной тишины, голос освобождается.
    {
        fresh();
        ViolinSynth_NoteOn(69, 100);
        CHECK(peak_of(render(4800)) > 0);
        CHECK(ViolinSynth_IsNoteActive(69));
        ViolinSynth_NoteOff(69);
        render(SR * 3);
        CHECK(!ViolinSynth_IsNoteActive(69));
        CHECK(peak_of(render(1024)) == 0);
    }

    // 3. Note On с velocity 0 = Note Off.
    {
        fresh();
        ViolinSynth_NoteOn(60, 90);
        render(4800);
        ViolinSynth_NoteOn(60, 0);
        render(SR * 3);
        CHECK(!ViolinSynth_IsNoteActive(60));
    }

    // 4. Строй: ±8 центов на 6 нотах по всему диапазону (вибрато выключено).
    {
        const int notes[] = {36, 48, 60, 72, 84, 96};
        for (int n : notes) {
            fresh();
            ViolinSynth_ControlChange(1, 0);
            ViolinSynth_NoteOn((uint8_t)n, 100);
            const auto b = render(SR * 2);
            const double f = f0_of(mono(b, SR, 24000), midi_hz(n));
            const double err = cents(f, midi_hz(n));
            std::printf("  note %d: %+.1f cents\n", n, err);
            CHECK(std::fabs(err) < 8.0);
        }
    }

    // 5. Pitch Bend: +2 полутона у верхнего положения колеса, симметрично вниз.
    {
        for (int dir = -1; dir <= 1; dir += 2) {
            fresh();
            ViolinSynth_ControlChange(1, 0);
            ViolinSynth_PitchBend(dir > 0 ? 8191 : -8192);
            ViolinSynth_NoteOn(60, 100);
            const auto b = render(SR * 2);
            const double err = cents(f0_of(mono(b, SR, 24000), midi_hz(60) * (dir > 0 ? 1.12 : 0.89)), midi_hz(60));
            std::printf("  bend %+d: %+.1f cents\n", dir, err);
            CHECK(std::fabs(err - dir * 200.0) < 20.0);
        }
    }

    // 6. Aftertouch усиливает смычок (громче), экспрессия CC11 ослабляет.
    {
        fresh();
        ViolinSynth_ControlChange(1, 0);
        ViolinSynth_NoteOn(64, 90);
        render(SR);
        const double base = rms_of(render(SR / 2));
        ViolinSynth_ChannelPressure(127);
        render(SR / 2);
        const double swell = rms_of(render(SR / 2));
        CHECK(swell > base * 1.1);

        ViolinSynth_ChannelPressure(0);
        ViolinSynth_ControlChange(11, 40);
        render(SR / 2);
        const double soft = rms_of(render(SR / 2));
        CHECK(soft < base * 0.8);
    }

    // 7. Яркость (CC74): больше - ярче.
    {
        double br[2];
        for (int i = 0; i < 2; ++i) {
            fresh();
            ViolinSynth_ControlChange(1, 0);
            ViolinSynth_ControlChange(74, i == 0 ? 0 : 127);
            ViolinSynth_NoteOn(57, 100);
            br[i] = brightness_of(mono(render(SR * 2), SR, 24000));
        }
        std::printf("  brightness: dark %.4f, bright %.4f\n", br[0], br[1]);
        CHECK(br[1] > br[0] * 1.2);
    }

    // 8. Вибрато (CC1 - мод-колесо ПРИБАВЛЯЕТ вибрато): в нуле - вибрато по умолчанию (как без CC1), при 127 - заметно
    //    сильнее.
    {
        double span[3];
        for (int i = 0; i < 3; ++i) {
            fresh();
            if (i < 2) ViolinSynth_ControlChange(1, i == 0 ? 0 : 127);   // i == 2: CC1 не трогаем
            ViolinSynth_NoteOn(64, 100);
            const auto b = render(SR * 3);
            double lo = 1e9, hi = -1e9;
            for (int w = 0; w < 14; ++w) {      // окна по 2048 отсчётов через 0.05 с, начиная с 1 с
                const double c = cents(f0_of(mono(b, SR + (size_t)w * 2400, 2048), midi_hz(64)), midi_hz(64));
                lo = std::min(lo, c);
                hi = std::max(hi, c);
            }
            span[i] = hi - lo;
        }
        std::printf("  vibrato span: wheel 0 %.1f, wheel 127 %.1f, untouched %.1f cents\n", span[0], span[1], span[2]);
        CHECK(span[0] == span[2]);          // колесо в нуле = колесо не трогали
        CHECK(span[1] > 25.0);
        CHECK(span[1] > span[0] + 8.0);     // колесо вверх добавляет вибрато
    }

    // 9. Педаль сустейна удерживает ноту после Note Off; отпускание педали - затухание.
    {
        fresh();
        ViolinSynth_NoteOn(62, 100);
        render(4800);
        ViolinSynth_ControlChange(64, 127);
        render(256);
        ViolinSynth_NoteOff(62);
        CHECK(peak_of(render(SR)) > 2000);
        CHECK(ViolinSynth_IsNoteActive(62));
        ViolinSynth_ControlChange(64, 0);
        render(SR * 3);
        CHECK(!ViolinSynth_IsNoteActive(62));
    }

    // 10. Полифония больше числа голосов: самая старая нота вытесняется, звук не клиппирует и не пропадает.
    {
        fresh();
        for (int i = 0; i < VIOLIN_VOICES + 4; ++i) ViolinSynth_NoteOn((uint8_t)(48 + 3 * i), 110);
        int peak = 0;
        for (int i = 0; i < 20; ++i) peak = std::max(peak, peak_of(render(2400)));
        CHECK(peak > 1000);
        CHECK(peak < 32767);
        CHECK(ViolinSynth_IsNoteActive((uint8_t)(48 + 3 * (VIOLIN_VOICES + 3))));   // последняя нота звучит
        CHECK(!ViolinSynth_IsNoteActive(48));                                       // первая вытеснена
    }

    // 11. Воспроизводимость: после Init два прогона одного сценария дают бит-в-бит одинаковый звук.
    {
        std::vector<int16_t> a, b;
        for (int run = 0; run < 2; ++run) {
            fresh();
            ViolinSynth_NoteOn(60, 100);
            ViolinSynth_NoteOn(67, 80);
            auto x = render(SR / 2);
            ViolinSynth_NoteOff(60);
            auto y = render(SR / 2);
            x.insert(x.end(), y.begin(), y.end());
            (run == 0 ? a : b) = x;
        }
        CHECK(a == b);
    }

    // 12. Ноты за пределами диапазона сворачиваются на октавы: звучат и не ломают модель.
    {
        const uint8_t notes[] = {0, 12, 127, 120};
        for (uint8_t n : notes) {
            fresh();
            ViolinSynth_NoteOn(n, 100);
            const auto b = render(SR / 2);
            CHECK(peak_of(b) > 0);
            CHECK(peak_of(b) < 32767);
        }
    }

    // 13. Лавина Note On без рендера: лишние отбрасываются (счётчик растёт), после рендера движок работает.
    {
        fresh();
        for (int i = 0; i < 200; ++i) ViolinSynth_NoteOn((uint8_t)(40 + i % 40), 100);
        CHECK(ViolinSynth_GetDroppedEventsCount() > 0);
        render(SR / 4);
        for (int i = 0; i < 200; ++i) ViolinSynth_NoteOff((uint8_t)(40 + i % 40));
        render(SR * 3);
        ViolinSynth_NoteOn(60, 100);
        CHECK(peak_of(render(SR / 4)) > 0);
    }

    // 14. Положение смычка (CC70) меняет тембр новых нот.
    {
        double br[2];
        for (int i = 0; i < 2; ++i) {
            fresh();
            ViolinSynth_ControlChange(1, 0);
            ViolinSynth_ControlChange(70, i == 0 ? 0 : 127);
            ViolinSynth_NoteOn(57, 100);
            br[i] = brightness_of(mono(render(SR * 2), SR, 24000));
        }
        CHECK(std::fabs(br[0] - br[1]) > 0.02 * std::max(br[0], br[1]));
    }

    if (purrmidi_test::Failures() == 0) std::puts("test_violin_host passed");
    return purrmidi_test::Finish("test_violin_host");
}

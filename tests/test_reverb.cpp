// Тесты общей реверберации (Engine/synth/reverb.h): сам алгоритм и ReverbLoadGuard, без диспетчера движков.
// Диспетчер (CC91, смена инструмента, All Sound Off, отключение под нагрузкой) проверяет test_engine_reverb.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "reverb.h"
#include "check.h"

namespace {

constexpr uint32_t kBlock = 128;
constexpr double   kFs    = 48000.0;
constexpr double   kPi    = 3.14159265358979323846;   // M_PI в MSVC без _USE_MATH_DEFINES недоступен

synth::Reverb g_rv;   // ~38 КБ: статический, как в прошивке

std::vector<int16_t> noise(uint32_t frames, int amplitude, uint32_t seed)
{
    std::mt19937                       rng(seed);
    std::uniform_int_distribution<int> d(-amplitude, amplitude);
    std::vector<int16_t>               v(2U * frames);
    for (auto &s : v) s = (int16_t)d(rng);
    return v;
}

void run_blocks(std::vector<int16_t> &io, float wet)
{
    for (size_t pos = 0; pos + 2U * kBlock <= io.size(); pos += 2U * kBlock) {
        g_rv.Process(io.data() + pos, kBlock, wet);
    }
}

double rms_range(const std::vector<int16_t> &v, size_t from, size_t to)
{
    double acc = 0.0;
    for (size_t i = from; i < to; ++i) acc += (double)v[i] * v[i];
    return std::sqrt(acc / (double)(to - from));
}

// Нулевая глубина: выход бит-в-бит равен входу, в том числе после того, как эффект работал.
void test_bypass_is_bit_exact()
{
    g_rv.Init();
    auto in  = noise(kBlock * 40U, 20000, 1);
    auto out = in;
    run_blocks(out, 0.0f);
    CHECK(std::memcmp(in.data(), out.data(), in.size() * sizeof(int16_t)) == 0);
    CHECK(!g_rv.IsActive());

    // Поработал и выключили: первый блок с нулевой целью плавно гасит мокрую часть, дальше - точный обход.
    auto warm = noise(kBlock * 100U, 8000, 2);
    run_blocks(warm, 0.8f);
    CHECK(g_rv.IsActive());
    auto a = noise(kBlock * 2U, 8000, 3);
    run_blocks(a, 0.0f);
    auto b  = noise(kBlock * 20U, 8000, 4);
    auto b0 = b;
    run_blocks(b, 0.0f);
    CHECK(std::memcmp(b0.data(), b.data(), b.size() * sizeof(int16_t)) == 0);
    CHECK(!g_rv.IsActive());
}

// Хвост после импульса: заметен, монотонно затухает и пропадает; время -60 дБ в разумных пределах.
void test_impulse_decay()
{
    g_rv.Init();
    std::vector<int16_t> io(2U * kBlock * (uint32_t)(4.0 * kFs / kBlock), 0);
    io[0] = io[1] = 20000;
    run_blocks(io, 1.0f);

    // Энергия по окнам в 100 мс.
    const size_t win = (size_t)(0.1 * kFs) * 2U;
    std::vector<double> e;
    for (size_t p = win; p + win <= io.size(); p += win) e.push_back(rms_range(io, p, p + win));
    CHECK(e.size() >= 30);
    CHECK(e[0] > 20.0);                 // хвост слышен сразу после импульса
    // Окна монотонно не растут (допуск 15% на биения мод).
    for (size_t i = 1; i < e.size(); ++i) CHECK(e[i] <= e[i - 1] * 1.15 + 1.0);

    // -60 дБ относительно первого окна: между 0,6 и 2,2 с (RT60 задан около 1,3 с с учётом потерь ВЧ).
    size_t i60 = 0;
    for (size_t i = 0; i < e.size(); ++i) {
        if (e[i] < e[0] * 0.001) { i60 = i; break; }
    }
    CHECK(i60 > 0);
    const double t60 = (double)i60 * 0.1;
    std::printf("  RT60 (по окнам 100 мс): %.1f c\n", t60);
    CHECK(t60 > 0.6 && t60 < 2.2);
}

// Устойчивость: 10 с полного шума не дают ни переполнений, ни роста; после тишины хвост уходит в ноль.
void test_stability_under_full_scale_noise()
{
    g_rv.Init();
    for (int sec = 0; sec < 10; ++sec) {
        auto io = noise((uint32_t)(kFs / kBlock) * kBlock, 32000, 10U + (uint32_t)sec);
        run_blocks(io, 1.0f);
        for (int16_t s : io) CHECK(s >= -32768 && s <= 32767);
    }
    std::vector<int16_t> quiet(2U * kBlock * (uint32_t)(12.0 * kFs / kBlock), 0);
    run_blocks(quiet, 1.0f);
    // Последняя секунда: полная тишина (хвост ушёл, денормали не зависают).
    const size_t last = (size_t)kFs * 2U;
    double peak = 0;
    for (size_t i = quiet.size() - last; i < quiet.size(); ++i) peak = std::max(peak, (double)std::abs(quiet[i]));
    std::printf("  пик последней секунды тишины: %.0f\n", peak);
    CHECK(peak <= 1.0);
}

// Уровень: на широкополосном шуме мокрый сигнал при усилении 1,0 примерно равен входу (+-2 дБ).
void test_wet_level_is_normalized()
{
    g_rv.Init();
    const auto in = noise(kBlock * 1500U, 3000, 5);
    auto       out = in;
    run_blocks(out, 1.0f);
    std::vector<int16_t> wet(in.size());
    for (size_t i = 0; i < in.size(); ++i) wet[i] = (int16_t)(out[i] - in[i]);
    const size_t from = in.size() / 2;
    const double ratio_db = 20.0 * std::log10(rms_range(wet, from, in.size()) / rms_range(in, from, in.size()));
    std::printf("  мокрый/вход: %+.1f дБ\n", ratio_db);
    CHECK(std::fabs(ratio_db) < 2.0);
}

// Стерео: мокрые каналы почти не коррелируют (широкий хвост), при моно-входе.
void test_stereo_decorrelation()
{
    g_rv.Init();
    auto mono = noise(kBlock * 1500U, 3000, 6);
    for (size_t i = 0; i < mono.size(); i += 2) mono[i + 1] = mono[i];
    auto out = mono;
    run_blocks(out, 1.0f);
    double sl = 0, sr = 0, slr = 0;
    for (size_t i = mono.size() / 2; i + 1 < mono.size(); i += 2) {
        const double l = out[i] - mono[i], r = out[i + 1] - mono[i + 1];
        sl += l * l; sr += r * r; slr += l * r;
    }
    const double corr = slr / std::sqrt(sl * sr);
    std::printf("  корреляция L/R мокрой части: %+.3f\n", corr);
    CHECK(std::fabs(corr) < 0.2);
}

// Щелчки: ступеньки глубины между блоками не добавляют разрывов (максимальный скачок соседних отсчётов).
void test_depth_changes_are_click_free()
{
    auto make_sine = [](uint32_t frames) {
        std::vector<int16_t> v(2U * frames);
        for (uint32_t i = 0; i < frames; ++i) {
            const int16_t s = (int16_t)(8000.0 * std::sin(2.0 * kPi * 220.0 * i / kFs));
            v[2U * i] = v[2U * i + 1U] = s;
        }
        return v;
    };
    auto max_step = [](const std::vector<int16_t> &v) {
        int m = 0;
        for (size_t i = 2; i < v.size(); i += 2) m = std::max(m, std::abs((int)v[i] - (int)v[i - 2]));
        return m;
    };

    const uint32_t frames = kBlock * 600U;
    g_rv.Init();
    auto steady = make_sine(frames);
    run_blocks(steady, 1.0f);   // эталон при наибольшей глубине из набора: сравниваем разрывы, а не громкость

    g_rv.Init();
    auto jumpy = make_sine(frames);
    const float targets[] = {0.0f, 0.9f, 0.1f, 1.0f, 0.0f, 0.6f};
    for (size_t blk = 0, k = 0; blk < frames / kBlock; ++blk) {
        if (blk % 50U == 0U) k = (k + 1U) % 6U;
        g_rv.Process(jumpy.data() + 2U * kBlock * blk, kBlock, targets[k]);
    }
    const int a = max_step(steady), b = max_step(jumpy);
    std::printf("  макс. скачок соседних отсчётов: ровная %d, со ступеньками %d\n", a, b);
    // Настоящий щелчок - скачок порядка уровня мокрого сигнала (тысячи единиц), рампа его не допускает.
    CHECK(b <= a + a / 4 + 50);
}

// Хвост не «воскресает»: выключили, подождали, включили при тишине на входе - выход ровно ноль.
void test_tail_does_not_resurrect()
{
    g_rv.Init();
    std::vector<int16_t> io(2U * kBlock * 50U, 0);
    io[0] = io[1] = 20000;
    run_blocks(io, 1.0f);
    std::vector<int16_t> off(2U * kBlock * 3U, 0);
    run_blocks(off, 0.0f);
    std::vector<int16_t> on(2U * kBlock * 50U, 0);
    run_blocks(on, 1.0f);
    for (int16_t s : on) CHECK(s == 0);
}

// All Sound Off: за ~21 мс хвост гаснет, затем состояние обнулено (ровный ноль).
void test_kill_fades_and_clears()
{
    g_rv.Init();
    auto io = noise(kBlock * 400U, 6000, 7);
    run_blocks(io, 1.0f);

    g_rv.Kill();
    std::vector<int16_t> q(2U * kBlock * 40U, 0);
    run_blocks(q, 1.0f);

    // Первые 128 кадров - ещё слышно, последние перед концом гашения - почти тишина.
    const double first = rms_range(q, 0, 2U * kBlock);
    const double late  = rms_range(q, 2U * (synth::Reverb::kKillSamples - kBlock), 2U * synth::Reverb::kKillSamples);
    std::printf("  Kill: RMS первого блока %.1f, последнего блока гашения %.1f\n", first, late);
    CHECK(first > 5.0);
    CHECK(late < first * 0.15);
    // После завершения гашения (с запасом в блок) - абсолютный ноль.
    const size_t done = 2U * (synth::Reverb::kKillSamples + kBlock);
    for (size_t i = done; i < q.size(); ++i) CHECK(q[i] == 0);

    // Вход во время гашения и паузы заглушён: звук движка, ещё затухающий после All Sound Off, хвоста не порождает.
    g_rv.Init();
    run_blocks(io = noise(kBlock * 400U, 6000, 7), 1.0f);
    g_rv.Kill();
    auto fading = noise(kBlock * 4U, 3000, 9);   // «последние отсчёты движка» на фоне гашения
    run_blocks(fading, 1.0f);
    std::vector<int16_t> after(2U * kBlock * 60U, 0);
    run_blocks(after, 1.0f);
    // Гашение (kKillSamples) началось на блоках fading; после его конца выход - ровный ноль.
    const size_t fade_left = 2U * (synth::Reverb::kKillSamples - 4U * kBlock);
    for (size_t i = fade_left; i < after.size(); ++i) CHECK(after[i] == 0);

    // Эффект остаётся рабочим после паузы: новый звук снова получает хвост.
    const uint32_t pause_blocks = (synth::Reverb::kKillSamples + synth::Reverb::kKillHoldSamples) / kBlock + 2U;
    std::vector<int16_t> gap(2U * kBlock * pause_blocks, 0);
    run_blocks(gap, 1.0f);
    std::vector<int16_t> imp(2U * kBlock * 40U, 0);
    imp[0] = imp[1] = 20000;
    run_blocks(imp, 1.0f);
    CHECK(rms_range(imp, 2U * kBlock * 10U, 2U * kBlock * 20U) > 5.0);
}

// Без Init() (нулевая память, как .bss) Process() безопасен: это обход.
void test_untouched_state_is_safe()
{
    static synth::Reverb fresh;   // статическая нулевая инициализация
    auto in  = noise(kBlock * 4U, 5000, 8);
    auto out = in;
    for (size_t pos = 0; pos + 2U * kBlock <= out.size(); pos += 2U * kBlock) fresh.Process(out.data() + pos, kBlock, 0.0f);
    CHECK(std::memcmp(in.data(), out.data(), in.size() * sizeof(int16_t)) == 0);
}

// Защита по нагрузке.
void test_load_guard()
{
    synth::ReverbLoadGuard g(70, 55, 2, 10);
    const uint32_t budget = 1000;

    // Нет часов (бюджет 0): всегда разрешено.
    CHECK(g.Update(999999, 0));
    CHECK(!g.IsShed());

    // Один тяжёлый блок - ещё не повод.
    CHECK(g.Update(800, budget));
    CHECK(g.Update(300, budget));        // счётчик подряд идущих сбрасывается
    CHECK(g.Update(800, budget));
    CHECK(!g.Update(800, budget));       // второй подряд: отключено
    CHECK(g.IsShed());

    // Между порогами (55..70%) возврата нет и счётчик покоя сбрасывается.
    for (int i = 0; i < 9; ++i) CHECK(!g.Update(300, budget));
    CHECK(!g.Update(650, budget));
    for (int i = 0; i < 9; ++i) CHECK(!g.Update(300, budget));
    CHECK(g.Update(300, budget));        // десятый спокойный блок подряд: вернули
    CHECK(!g.IsShed());

    // Ровно на пороге (70%) не считается перегрузкой.
    synth::ReverbLoadGuard h(70, 55, 2, 10);
    for (int i = 0; i < 20; ++i) CHECK(h.Update(700, budget));
}

}  // namespace

int main()
{
    test_untouched_state_is_safe();
    test_bypass_is_bit_exact();
    test_impulse_decay();
    test_stability_under_full_scale_noise();
    test_wet_level_is_normalized();
    test_stereo_decorrelation();
    test_depth_changes_are_click_free();
    test_tail_does_not_resurrect();
    test_kill_fades_and_clears();
    test_load_guard();
    if (purrmidi_test::Failures() != 0) {
        std::fprintf(stderr, "test_reverb: %d check(s) failed\n", purrmidi_test::Failures());
        return 1;
    }
    std::puts("test_reverb: OK");
    return 0;
}

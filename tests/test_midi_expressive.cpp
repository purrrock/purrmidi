// Pitch Bend / Channel Pressure / Polyphonic Key Pressure: маршрут MIDI_Dispatch -> SynthEngine -> движок.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "midi_dispatch.h"
#include "synth_engine.h"
#include "check.h"

namespace {

constexpr uint32_t SR = 48000;

void send(uint8_t status, uint8_t d1, uint8_t d2)
{
    MIDI_Event_t e{};
    e.status = status;
    e.data1  = d1;
    e.data2  = d2;
    MIDI_Dispatch(&e);
}

void bend(int value14)   // 0..16383, центр 8192
{
    send(MIDI_STATUS_PITCH_BEND, (uint8_t)(value14 & 0x7F), (uint8_t)((value14 >> 7) & 0x7F));
}

std::vector<int16_t> render(uint32_t frames)
{
    std::vector<int16_t> out(frames * 2);
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = (frames - done) > 128 ? 128 : (frames - done);
        SynthEngine_FillStereoBuffer(out.data() + done * 2, n);
        done += n;
    }
    return out;
}

uint64_t hash_of(const std::vector<int16_t> &b)
{
    uint64_t h = 1469598103934665603ULL;
    for (int16_t s : b) { h ^= (uint16_t)s; h *= 1099511628211ULL; }
    return h;
}

// Частота по автокорреляции левого канала (окно 4096 с начала буфера), поиск в пределах ±30% от ожидаемой.
double estimate_f0(const std::vector<int16_t> &b, double f_expected)
{
    const size_t win = 4096;
    const int lo = (int)(SR / (f_expected * 1.3));
    const int hi = (int)(SR / (f_expected * 0.7));
    std::vector<double> x(win + hi + 2);
    for (size_t i = 0; i < x.size() && i * 2 < b.size(); ++i) x[i] = b[i * 2];
    double best = -1e300; int best_lag = lo;
    std::vector<double> r(hi + 2, 0.0);
    for (int lag = lo - 1; lag <= hi + 1; ++lag) {
        double acc = 0;
        for (size_t i = 0; i < win; ++i) acc += x[i] * x[i + lag];
        r[lag] = acc;
    }
    for (int lag = lo; lag <= hi; ++lag) if (r[lag] > best) { best = r[lag]; best_lag = lag; }
    const double a = r[best_lag - 1], c = r[best_lag + 1];
    const double denom = a - 2 * best + c;
    const double shift = denom != 0 ? 0.5 * (a - c) / denom : 0.0;
    return SR / (best_lag + shift);
}

double measure(double f_expected)
{
    render(SR / 50);
    return estimate_f0(render(SR / 4), f_expected);
}

void select_epiano_fresh()
{
    SynthEngine_Select(SYNTH_ENGINE_PLUCK);
    SynthEngine_Select(SYNTH_ENGINE_EPIANO);   // новый Init
}

void test_dispatch_pitch_bend_epiano()
{
    SynthEngine_Init();
    select_epiano_fresh();
    send(MIDI_STATUS_NOTE_ON, 69, 100);
    const double f0 = measure(440.0);
    bend(16383);
    const double up = measure(494.0);
    bend(0);
    const double down = measure(392.0);
    bend(8192);
    const double back = measure(440.0);
    std::printf("epiano bend: %.1f %.1f %.1f %.1f\n", f0, up, down, back);
    CHECK(std::fabs(f0 - 440.0) < 5.0);
    CHECK(std::fabs(up - 440.0 * std::pow(2.0, 2.0 / 12.0)) < 8.0);
    CHECK(std::fabs(down - 440.0 * std::pow(2.0, -2.0 / 12.0)) < 8.0);
    CHECK(std::fabs(back - 440.0) < 5.0);
    send(MIDI_STATUS_NOTE_OFF, 69, 0);
}

// Удерживаемое колесо переживает смену движка; Panic (отключение клавиатуры) возвращает его в центр.
void test_bend_survives_engine_switch_and_panic()
{
    SynthEngine_Init();
    select_epiano_fresh();
    bend(16383);
    select_epiano_fresh();                       // Init движка сбросил бы колесо, если бы его не переносили
    send(MIDI_STATUS_NOTE_ON, 69, 100);
    const double held = measure(494.0);
    CHECK(std::fabs(held - 493.9) < 8.0);

    SynthEngine_Panic();
    send(MIDI_STATUS_NOTE_ON, 69, 100);
    const double after_panic = measure(440.0);
    CHECK(std::fabs(after_panic - 440.0) < 5.0);
}

// Движки без поддержки Pitch Bend / Aftertouch игнорируют их: звук бит-в-бит такой же, как без этих сообщений.
uint64_t run_scenario(uint8_t engine, bool with_messages)
{
    SynthEngine_Select(SYNTH_ENGINE_EPIANO);
    std::srand(12345);
    SynthEngine_Select((SynthEngineId)engine);
    send(MIDI_STATUS_NOTE_ON, 60, 100);
    send(MIDI_STATUS_NOTE_ON, 67, 90);
    std::vector<int16_t> all;
    for (int i = 0; i < 20; ++i) {
        if (with_messages && i == 2) {
            bend(16383);
            send(MIDI_STATUS_CHANNEL_PRESSURE, 100, 0);
            send(MIDI_STATUS_POLY_KEY_PRESSURE, 60, 90);
        }
        if (with_messages && i == 10) bend(8192);
        auto b = render(128);
        all.insert(all.end(), b.begin(), b.end());
    }
    send(MIDI_STATUS_NOTE_OFF, 60, 0);
    send(MIDI_STATUS_NOTE_OFF, 67, 0);
    SynthEngine_Panic();
    return hash_of(all);
}

void test_unsupported_engines_ignore_messages()
{
    SynthEngine_Init();
    const SynthEngineId engines[] = { SYNTH_ENGINE_PLUCK, SYNTH_ENGINE_LEAD, SYNTH_ENGINE_ORGAN,
                                          SYNTH_ENGINE_DULCIMER, SYNTH_ENGINE_FLUTE, SYNTH_ENGINE_SAXOPHONE };
    for (SynthEngineId e : engines) {
        const uint64_t a = run_scenario(e, false);
        const uint64_t b = run_scenario(e, true);
        std::printf("engine %d: %016llx %016llx\n", (int)e, (unsigned long long)a, (unsigned long long)b);
        CHECK(a == b);
    }
}

// Aftertouch не меняет звук и у E-Piano (движок его не использует), мусорные значения не ломают диспетчер.
void test_pressure_and_garbage_are_harmless()
{
    SynthEngine_Init();
    select_epiano_fresh();
    send(MIDI_STATUS_NOTE_ON, 60, 100);
    const auto ref = render(1024);
    select_epiano_fresh();
    send(MIDI_STATUS_NOTE_ON, 60, 100);
    send(MIDI_STATUS_CHANNEL_PRESSURE, 127, 0);
    send(MIDI_STATUS_POLY_KEY_PRESSURE, 60, 127);
    send(MIDI_STATUS_POLY_KEY_PRESSURE, 255, 255);   // нота вне диапазона
    send(MIDI_STATUS_CHANNEL_PRESSURE, 255, 0);
    const auto got = render(1024);
    CHECK(hash_of(ref) == hash_of(got));
}

}  // namespace

int main()
{
    test_dispatch_pitch_bend_epiano();
    test_bend_survives_engine_switch_and_panic();
    test_unsupported_engines_ignore_messages();
    test_pressure_and_garbage_are_harmless();
    if (purrmidi_test::Failures() == 0) std::puts("test_midi_expressive passed");
    return purrmidi_test::Finish("test_midi_expressive");
}

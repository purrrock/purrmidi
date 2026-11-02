// Общая реверберация на уровне диспетчера SynthEngine: CC91, независимость от инструмента, сброс хвоста
// (Panic / смена инструмента / All Sound Off / громкость 0) и отключение под нагрузкой.
// Сам алгоритм проверяет test_reverb.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "midi_dispatch.h"
#include "synth_engine.h"
#include "check.h"

namespace {

constexpr uint32_t kBlock = 128;

void send(uint8_t status, uint8_t d1, uint8_t d2, uint8_t channel = 0)
{
    MIDI_Event_t e = {static_cast<uint8_t>(status | channel), d1, d2};
    MIDI_Dispatch(&e);
}
void cc(uint8_t c, uint8_t v, uint8_t ch = 0) { send(MIDI_STATUS_CONTROL_CHANGE, c, v, ch); }
void pc(uint8_t p)                           { send(MIDI_STATUS_PROGRAM_CHANGE, p, 0); }
void note_on(uint8_t n, uint8_t v = 100)     { send(MIDI_STATUS_NOTE_ON, n, v); }
void note_off(uint8_t n)                     { send(MIDI_STATUS_NOTE_OFF, n, 0); }

// Рендерит blocks блоков по 128 кадров и возвращает все отсчёты.
std::vector<int16_t> render(uint32_t blocks)
{
    std::vector<int16_t> out;
    std::vector<int16_t> buf(2U * kBlock);
    for (uint32_t b = 0; b < blocks; ++b) {
        SynthEngine_FillStereoBuffer(buf.data(), kBlock);
        out.insert(out.end(), buf.begin(), buf.end());
    }
    return out;
}

bool all_zero(const std::vector<int16_t> &v, size_t from = 0)
{
    for (size_t i = from; i < v.size(); ++i) if (v[i] != 0) return false;
    return true;
}

double rms(const std::vector<int16_t> &v, size_t from, size_t to)
{
    double acc = 0.0;
    for (size_t i = from; i < to; ++i) acc += (double)v[i] * v[i];
    return std::sqrt(acc / (double)(to - from));
}

constexpr uint32_t kBlocksPerSec = 48000U / kBlock;

// Движки без SoundFont (ему нужен банк; с ним то же самое проверяет test_engine_unified через общий путь).
const uint8_t kPrograms[] = {0, 1, 2, 3, 4};

void test_defaults_and_api()
{
    SynthEngine_Init();
    CHECK(SYNTH_REVERB_DEFAULT == 40);
    CHECK(SynthEngine_GetReverb() == SYNTH_REVERB_DEFAULT);

    cc(SYNTH_REVERB_CC, 90, 5);                  // канал не важен
    CHECK(SynthEngine_GetReverb() == 90);
    SynthEngine_SetReverb(200);                  // сверху ограничивается
    CHECK(SynthEngine_GetReverb() == 127);
    SynthEngine_SetReverb(0);
    CHECK(SynthEngine_GetReverb() == 0);

    // Как громкость: не сбрасывается сменой инструмента, Panic, All Notes/Sound Off.
    cc(SYNTH_REVERB_CC, 77);
    for (uint8_t p : kPrograms) {
        pc(p);
        CHECK(SynthEngine_GetReverb() == 77);
    }
    SynthEngine_Panic();
    CHECK(SynthEngine_GetReverb() == 77);
    cc(123, 0);
    cc(120, 0);
    CHECK(SynthEngine_GetReverb() == 77);

    // После включения питания (Init) всегда значение по умолчанию: настройка не переживает выключение.
    SynthEngine_Init();
    CHECK(SynthEngine_GetReverb() == SYNTH_REVERB_DEFAULT);
}

// Глубина 0: после затухания ноты выход ровно ноль; при большой глубине хвост ещё слышен.
void test_tail_depends_on_depth()
{
    for (uint8_t p : kPrograms) {
        double tail[2] = {0, 0};
        const uint8_t depths[2] = {0, 127};
        for (int k = 0; k < 2; ++k) {
            SynthEngine_Init();
            pc(p);
            cc(SYNTH_REVERB_CC, depths[k]);
            note_on(69, 110);
            render(kBlocksPerSec / 4);
            // All Sound Off здесь не годится (он гасит и хвост): отпускаем ноту штатно и ждём.
            note_off(69);
            render(kBlocksPerSec / 2);
            const auto t = render(kBlocksPerSec / 2);
            tail[k] = rms(t, 0, t.size());
        }
        std::printf("  программа %u: RMS через 0,5-1 с после Note Off: глубина 0 -> %.1f, 127 -> %.1f\n", p, tail[0], tail[1]);
        CHECK(tail[1] > tail[0] + 5.0);   // хвост реверберации слышен
    }
}

// All Sound Off гасит и хвост реверберации. Сами движки гаснут по-разному (Pluck доигрывает до ~110 мс, у Violin остаётся
// тихий резонанс деки), поэтому проверяется вклад эффекта: через 250 мс пик при максимальной глубине не больше, чем втрое
// выше пика того же движка без реверберации (плюс запас на округление). Без паузы на входе хвост от остатков движка
// оставался бы на секунду и тянул бы за собой десятки процентов предыдущего уровня.
void test_all_sound_off_silences_tail()
{
    for (uint8_t p : kPrograms) {
        int peak[2] = {0, 0};
        const uint8_t depths[2] = {0, 127};
        for (int k = 0; k < 2; ++k) {
            SynthEngine_Init();
            pc(p);
            cc(SYNTH_REVERB_CC, depths[k]);
            note_on(60); note_on(64); note_on(67);
            render(kBlocksPerSec / 2);
            const auto before = render(8);
            cc(120, 0);
            render(kBlocksPerSec / 4);                       // 250 мс
            const auto after = render(kBlocksPerSec / 4);    // следующие 250 мс
            for (int16_t s : after) peak[k] = std::max(peak[k], std::abs((int)s));
            if (k == 1) {
                int pk = 0;
                for (int16_t s : before) pk = std::max(pk, std::abs((int)s));
                std::printf("  программа %u: пик до All Sound Off %d, через 250 мс: без реверберации %d, с ней %d\n", p, pk, peak[0], peak[1]);
                CHECK(peak[1] < pk / 20 + 1);                // хвост предыдущего звука ушёл
            }
        }
        CHECK(peak[1] <= peak[0] * 3 + 4);
    }
}

// Panic и смена инструмента не переносят хвост в новый звук.
void test_panic_and_switch_drop_the_tail()
{
    for (uint8_t p : kPrograms) {
        SynthEngine_Init();
        pc(p);
        cc(SYNTH_REVERB_CC, 127);
        note_on(60); note_on(67);
        render(kBlocksPerSec / 4);
        SynthEngine_Panic();
        CHECK(all_zero(render(kBlocksPerSec / 4)));

        note_on(60);
        render(kBlocksPerSec / 4);
        pc((uint8_t)((p + 1U) % 5U));
        CHECK(all_zero(render(kBlocksPerSec / 4)));
    }
}

// Громкость 0 заглушает и хвост (реверберация стоит до регулятора громкости).
void test_master_volume_scales_tail()
{
    SynthEngine_Init();
    pc(2);
    cc(SYNTH_REVERB_CC, 127);
    note_on(69);
    render(kBlocksPerSec / 4);
    note_off(69);
    render(kBlocksPerSec / 10);
    const auto before = render(kBlocksPerSec / 10);
    CHECK(rms(before, 0, before.size()) > 5.0);   // хвост есть
    cc(SYNTH_MASTER_VOLUME_CC, 0);
    render(2);
    CHECK(all_zero(render(kBlocksPerSec / 5)));
}

// Тихий вход при максимальной глубине не вызывает переполнений: плотный громкий аккорд остаётся в int16.
void test_no_overflow_on_loud_chords()
{
    for (uint8_t p : kPrograms) {
        SynthEngine_Init();
        pc(p);
        cc(SYNTH_REVERB_CC, 127);
        for (uint8_t n = 36; n < 84; n += 3) note_on(n, 127);
        const auto out = render(kBlocksPerSec);
        int peak = 0;
        for (int16_t s : out) peak = std::max(peak, std::abs((int)s));
        CHECK(peak <= 32767);
    }
}

// Часы для проверки защиты: каждый вызов продвигает счётчик на фиксированный шаг (разница между двумя вызовами
// вокруг рендера движка = шаг = «время рендера» в тактах).
uint32_t g_clock = 0;
uint32_t g_step  = 0;
uint32_t fake_now(void) { g_clock += g_step; return g_clock; }

// Нота на Lead; 60 блоков при «спокойных» часах (эффект набирает хвост), затем 40 блоков при заданных часах.
std::vector<int16_t> scenario(uint8_t depth, uint32_t step_after, uint32_t blocks_after)
{
    SynthEngine_Init();
    pc(2);
    cc(SYNTH_REVERB_CC, depth);
    g_clock = 0;
    g_step  = 300;
    note_on(69, 110);
    render(60);
    g_step = step_after;
    return render(blocks_after);
}

void test_overload_drops_reverb_first()
{
    SynthEngine_SetClockForTest(fake_now, 1000);   // бюджет блока 1000 тактов

    // Спокойно (30% бюджета): реверберация работает и отличается от выключенной.
    const auto calm     = scenario(127, 300, 40);
    const auto calm_dry = scenario(0, 300, 40);
    CHECK(!SynthEngine_IsReverbShed());
    CHECK(std::memcmp(calm.data(), calm_dry.data(), calm.size() * sizeof(int16_t)) != 0);

    // Тяжело (80% бюджета): через два блока эффект отключается и дальше выход бит-в-бит как при глубине 0
    // (блоки: два до решения, один плавного затухания, один сброса).
    const auto shed = scenario(127, 800, 40);
    CHECK(SynthEngine_IsReverbShed());
    const auto dry = scenario(0, 800, 40);
    CHECK(shed.size() == dry.size());
    const size_t skip = 2U * kBlock * 4U;
    CHECK(std::memcmp(shed.data(), dry.data(), skip * sizeof(int16_t)) != 0);   // до решения хвост ещё звучал
    CHECK(std::memcmp(shed.data() + skip, dry.data() + skip, (shed.size() - skip) * sizeof(int16_t)) == 0);

    // Возврат: между порогами (55..70% бюджета) эффект остаётся выключенным, после 128 спокойных блоков подряд - включается.
    g_step = 600;
    render(200);
    CHECK(SynthEngine_IsReverbShed());
    g_step = 300;
    render(127);
    CHECK(SynthEngine_IsReverbShed());
    render(2);
    CHECK(!SynthEngine_IsReverbShed());

    // Смена инструмента сбрасывает защиту: новый движок получает эффект сразу (его нагрузку ещё не мерили).
    g_step = 800;
    render(10);
    CHECK(SynthEngine_IsReverbShed());
    pc(0);
    CHECK(!SynthEngine_IsReverbShed());

    // Без часов (как на ПК) защита выключена.
    SynthEngine_SetClockForTest(nullptr, 0);
    SynthEngine_Init();
    render(50);
    CHECK(!SynthEngine_IsReverbShed());
}

}  // namespace

int main()
{
    test_defaults_and_api();
    test_tail_depends_on_depth();
    test_all_sound_off_silences_tail();
    test_panic_and_switch_drop_the_tail();
    test_master_volume_scales_tail();
    test_no_overflow_on_loud_chords();
    test_overload_drops_reverb_first();
    if (purrmidi_test::Failures() != 0) {
        std::fprintf(stderr, "test_engine_reverb: %d check(s) failed\n", purrmidi_test::Failures());
        return 1;
    }
    std::puts("test_engine_reverb: OK");
    return 0;
}

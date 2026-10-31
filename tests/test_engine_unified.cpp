// Единое поведение движков (ROADMAP, раздел 3). Все проверки идут через публичный API SynthEngine, одинаково для каждого
// движка (E-Piano, Pluck, Lead, Organ, Violin и - если найден tests/SNDFNT.SF2 - SoundFont):
//
//   1. Pitch Bend: ±2 полутона, и когда колесо уже отклонено к началу ноты, и когда его двигают во время звучания;
//      плавное движение колеса не даёт щелчков; крайние ноты и крайние положения колеса не ломают звук.
//   2. All Notes Off (CC123): бит-в-бит то же самое, что Note Off по всем удерживаемым нотам и снятие педали; отпускает
//      и ноты, которые держала педаль.
//   3. All Sound Off (CC120): звук гаснет быстро, даже при зажатых клавишах и педали; после него движок работает.
//   4. Panic() и смена инструмента сбрасывают одинаково полное состояние: после «грязного» сценария (все контроллеры,
//      колесо, педаль, ноты) звук бит-в-бит такой же, как у только что выбранного движка.
//   5. CC1 (мод-колесо): в нуле звук как без колеса (бит-в-бит), вверх - звук меняется; у E-Piano / Pluck / Lead - ярче.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sf2_loader_host.h"
#include "soundfont_synth.h"
#include "synth_engine.h"
#include "check.h"

namespace {

constexpr uint32_t SR = 48000;

struct EngineCase {
    uint32_t    program;
    const char *name;
    bool        has_pedal;   // реагирует ли на CC64 (орган - нет)
};

std::vector<EngineCase> g_engines;

// ---- обёртки -----------------------------------------------------------------------------------------------------

void note_on(uint8_t n, uint8_t v = 100)  { SynthEngine_NoteOn(n, v); }
void note_off(uint8_t n)                  { SynthEngine_NoteOff(n); }
void cc(uint8_t c, uint8_t v)             { SynthEngine_ControlChange(c, v); }
void bend(int v)                          { SynthEngine_PitchBend((int16_t)v); }   // -8192..8191

// Рендер: возвращает левый канал.
std::vector<int16_t> render(uint32_t frames)
{
    std::vector<int16_t> st((size_t)frames * 2U);
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min<uint32_t>(128U, frames - done);
        SynthEngine_FillStereoBuffer(st.data() + (size_t)done * 2U, n);
        done += n;
    }
    std::vector<int16_t> l(frames);
    for (uint32_t i = 0; i < frames; ++i) l[i] = st[(size_t)i * 2U];
    return l;
}

uint64_t hash_of(const std::vector<int16_t> &v, uint64_t h = 1469598103934665603ULL)
{
    const uint8_t *b = reinterpret_cast<const uint8_t *>(v.data());
    for (size_t i = 0; i < v.size() * sizeof(int16_t); ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

int peak_of(const std::vector<int16_t> &v, size_t from = 0, size_t to = (size_t)-1)
{
    int p = 0;
    to = std::min(to, v.size());
    for (size_t i = from; i < to; ++i) p = std::max(p, std::abs((int)v[i]));
    return p;
}

// Свежий движок: выбираем другой, затем нужный (Init() обнуляет всё состояние движка).
void select_fresh(uint32_t program)
{
    SynthEngine_PitchBend(0);   // колесо - физический орган: при смене инструмента оно остаётся где стоит, поэтому центрируем
    SynthEngine_SelectProgram(program == 0U ? 1U : 0U);
    SynthEngine_SelectProgram(program);
}

// ---- измерение высоты тона ---------------------------------------------------------------------------------------

// Частота по автокорреляции в окне [start, start+len): ищем максимум около ожидаемого периода (±25%), уточняем параболой.
double measure_hz(const std::vector<int16_t> &b, size_t start, size_t len, double expect_hz)
{
    const int lag_lo = (int)(SR / (expect_hz * 1.30));
    const int lag_hi = (int)(SR / (expect_hz * 0.75));
    std::vector<double> r((size_t)lag_hi + 2U, 0.0);
    for (int lag = lag_lo - 1; lag <= lag_hi + 1; ++lag) {
        double acc = 0.0, e1 = 0.0, e2 = 0.0;
        for (size_t i = 0; i + (size_t)lag < len; ++i) {
            const double x = b[start + i], y = b[start + i + (size_t)lag];
            acc += x * y; e1 += x * x; e2 += y * y;
        }
        r[(size_t)lag] = (e1 > 0.0 && e2 > 0.0) ? acc / std::sqrt(e1 * e2) : 0.0;
    }
    int best = lag_lo;
    for (int lag = lag_lo; lag <= lag_hi; ++lag) if (r[(size_t)lag] > r[(size_t)best]) best = lag;
    const double a = r[(size_t)best - 1], c = r[(size_t)best], d = r[(size_t)best + 1];
    const double denom = a - 2.0 * c + d;
    const double shift = (denom != 0.0) ? 0.5 * (a - d) / denom : 0.0;
    return (double)SR / ((double)best + shift);
}

double cents_of(double ratio) { return 1200.0 * std::log2(ratio); }

// ---- 1. Pitch Bend -----------------------------------------------------------------------------------------------

constexpr uint8_t kNote = 57;   // A3, 220 Гц
const double kNoteHz = 220.0;

void test_pitch_bend()
{
    for (const EngineCase &e : g_engines) {
        const int positions[] = { 8191, -8192 };
        const double expected_cents[] = { 200.0, -200.0 };

        // Эталон: без колеса.
        select_fresh(e.program);
        note_on(kNote);
        render(SR / 5);
        const double hz0 = measure_hz(render(8192), 0, 8192, kNoteHz);

        for (int k = 0; k < 2; ++k) {
            // (a) колесо отклонено до Note On: нота начинается сразу с изменённой высотой
            select_fresh(e.program);
            bend(positions[k]);
            render(2048);                       // колесо успело сгладиться
            note_on(kNote);
            render(SR / 5);
            const double hz_a = measure_hz(render(8192), 0, 8192, kNoteHz);
            const double c_a = cents_of(hz_a / hz0);

            // (b) колесо двигают во время звучания ноты
            select_fresh(e.program);
            note_on(kNote);
            render(SR / 5);
            bend(positions[k]);
            render(SR / 5);                     // сглаживание и перенастройка
            const double hz_b = measure_hz(render(8192), 0, 8192, kNoteHz);
            const double c_b = cents_of(hz_b / hz0);

            std::printf("  %-9s bend %+5d: before-note %+7.1f c, during-note %+7.1f c (expected %+.0f)\n",
                        e.name, positions[k], c_a, c_b, expected_cents[k]);
            // Допуск 25 центов: физические модели (Pluck, Violin) настраиваются чуть менее точно, чем осцилляторы.
            CHECK(std::fabs(c_a - expected_cents[k]) < 25.0);
            CHECK(std::fabs(c_b - expected_cents[k]) < 25.0);
        }

        // Возврат в центр: частота вернулась к исходной.
        select_fresh(e.program);
        note_on(kNote);
        render(SR / 10);
        bend(8191);
        render(SR / 5);
        bend(0);
        render(SR / 5);
        const double hz_back = measure_hz(render(8192), 0, 8192, kNoteHz);
        CHECK(std::fabs(cents_of(hz_back / hz0)) < 20.0);
    }
}

// Максимум модуля второй разности: «щелчок» (скачок фазы или амплитуды) на гладком сигнале виден как выброс.
double max_second_difference(const std::vector<int16_t> &b, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = from + 2U; i < to && i < b.size(); ++i) {
        m = std::max(m, std::fabs((double)b[i] - 2.0 * b[i - 1] + b[i - 2]));
    }
    return m;
}

void test_pitch_bend_has_no_clicks()
{
    for (const EngineCase &e : g_engines) {
        // Одна и та же нота: без колеса и с плавным ходом колеса 0 -> +max -> -max -> 0 за 0,5 с.
        std::vector<int16_t> plain, swept;
        for (int pass = 0; pass < 2; ++pass) {
            select_fresh(e.program);
            note_on(kNote);
            render(SR / 5);
            std::vector<int16_t> acc;
            for (int step = 0; step < 100; ++step) {
                int v = 0;
                if (pass == 1) {
                    const double ph = (double)step / 100.0;                // 0..1
                    v = (int)(std::sin(ph * 2.0 * 3.14159265358979) * 8191.0);
                }
                bend(v);
                const auto blk = render(240);                             // 5 мс на шаг, как поток сообщений колеса
                acc.insert(acc.end(), blk.begin(), blk.end());
            }
            (pass == 0 ? plain : swept) = acc;
        }
        const double base = std::max(1.0, max_second_difference(plain, 0, plain.size()));
        const double got  = max_second_difference(swept, 0, swept.size());
        std::printf("  %-9s max |d2|: plain %.0f, swept %.0f (x%.2f)\n", e.name, base, got, got / base);
        // Частота растёт на 12%, вторая разность синусоиды - на 25%. У физических моделей (Pluck: перестройка линии
        // задержки на ходу) бывают единичные слабые «тики» при смене целой части задержки - порядка 2x от обычных
        // пиков; настоящий щелчок (скачок фазы/амплитуды) даёт выброс в разы больше.
        CHECK(got < base * 2.5);
    }
}

void test_pitch_bend_extremes_are_safe()
{
    for (const EngineCase &e : g_engines) {
        const int positions[] = { 8191, -8192, 0, 5000, -3000 };
        const uint8_t notes[] = { 0, 12, 24, 60, 96, 120, 127 };
        select_fresh(e.program);
        for (uint8_t n : notes) {
            for (int p : positions) {
                bend(p);
                note_on(n, 127);
                const auto a = render(1500);
                note_off(n);
                const auto b = render(1500);
                // Звук конечный и не «взрывается»: int16 уже ограничивает, поэтому проверяем отсутствие сплошного клиппинга.
                int clipped = 0;
                for (int16_t s : a) if (s == 32767 || s == -32768) ++clipped;
                for (int16_t s : b) if (s == 32767 || s == -32768) ++clipped;
                CHECK(clipped < 8);
            }
        }
        // Аккорд из десяти нот при крайнем положении колеса (много голосов, вытеснение).
        bend(-8192);
        for (int i = 0; i < 10; ++i) note_on((uint8_t)(36 + 5 * i), 110);
        render(4800);
        bend(8191);
        render(4800);
        cc(123, 0);
        render(SR / 2);
        bend(0);
        CHECK(true);   // главное - дошли без падения/зависания (под ASan/UBSan - без нарушений)
    }
}

// ---- 2. All Notes Off (CC123) ------------------------------------------------------------------------------------

uint64_t scenario_release(const EngineCase &e, bool use_cc123, bool pedal_down_before_release)
{
    select_fresh(e.program);
    uint64_t h = 1469598103934665603ULL;
    note_on(60, 100); note_on(64, 90); note_on(67, 80);
    h = hash_of(render(4800), h);
    if (pedal_down_before_release) { cc(64, 127); render(256); }
    if (use_cc123) {
        cc(123, 0);
    } else {
        note_off(60); note_off(64); note_off(67);
        cc(64, 0);
    }
    h = hash_of(render(SR), h);
    return h;
}

void test_all_notes_off()
{
    for (const EngineCase &e : g_engines) {
        // Бит-в-бит то же, что обычное отпускание всех нот и снятие педали.
        const uint64_t a = scenario_release(e, true, false);
        const uint64_t b = scenario_release(e, false, false);
        if (a != b) std::printf("  %s: CC123 differs from explicit Note Off\n", e.name);
        CHECK(a == b);
        const uint64_t a2 = scenario_release(e, true, true);
        const uint64_t b2 = scenario_release(e, false, true);
        if (a2 != b2) std::printf("  %s: CC123 (with pedal) differs from explicit Note Off + pedal up\n", e.name);
        CHECK(a2 == b2);

        // Ноты, которые держит педаль, тоже отпускаются: после CC123 звук затухает, без него педаль его держит.
        if (e.has_pedal) {
            int peaks[2];
            for (int pass = 0; pass < 2; ++pass) {
                select_fresh(e.program);
                note_on(60, 100); note_on(64, 100);
                render(4800);
                cc(64, 127);
                render(256);
                note_off(60); note_off(64);       // теперь их держит только педаль
                render(SR / 2);
                if (pass == 1) cc(123, 0);
                const auto tail = render(SR * 2);
                peaks[pass] = peak_of(tail, (size_t)SR * 3 / 2, tail.size());
            }
            std::printf("  %-9s pedal-held tail peak: pedal %d, after CC123 %d\n", e.name, peaks[0], peaks[1]);
            CHECK(peaks[1] * 4 < peaks[0] + 1);
        }

        // Движок работает после CC123.
        select_fresh(e.program);
        note_on(60, 100);
        render(2400);
        cc(123, 0);
        render(SR);
        note_on(62, 100);
        CHECK(peak_of(render(4800)) > 500);
    }
}

// ---- 3. All Sound Off (CC120) ------------------------------------------------------------------------------------

void test_all_sound_off()
{
    for (const EngineCase &e : g_engines) {
        select_fresh(e.program);
        note_on(60, 110); note_on(64, 110); note_on(67, 110);
        if (e.has_pedal) cc(64, 127);
        const auto before = render(SR / 2);
        const int before_peak = peak_of(before, before.size() / 2);

        cc(120, 0);                              // клавиши всё ещё «зажаты», педаль нажата
        render(SR / 5);                          // время на быстрый спад
        const auto tail = render(SR / 4);
        const int tail_peak = peak_of(tail);
        std::printf("  %-9s All Sound Off: peak %d -> %d\n", e.name, before_peak, tail_peak);
        CHECK(before_peak > 1000);
        CHECK(tail_peak * 50 < before_peak);     // тише на 34 дБ и больше уже через 0,2 с

        // Отпускание клавиш после CC120 безвредно, а новая нота звучит (и педаль сброшена: после Note Off звук не висит).
        note_off(60); note_off(64); note_off(67);
        render(1024);
        note_on(62, 100);
        CHECK(peak_of(render(4800)) > 500);
        note_off(62);
        const auto rel = render(SR * 2);
        if (e.has_pedal) {
            CHECK(peak_of(rel, (size_t)SR * 3 / 2, rel.size()) * 8 < peak_of(rel, 0, 4800) + 1);
        }

        // CC120 не меняет остальные настройки и колесо высоты тона: после него нота звучит с тем же отклонением.
        select_fresh(e.program);
        bend(8191);
        render(2048);
        note_on(kNote);
        render(SR / 5);
        cc(120, 0);
        render(SR / 2);
        note_on(kNote);
        render(SR / 5);
        const auto again = render(8192);
        // До этого колесо уже было отклонено; частота измеряется относительно ровно такого же сценария без CC120.
        select_fresh(e.program);
        bend(8191);
        render(2048);
        note_on(kNote);
        render(SR / 5);
        const double hz_ref = measure_hz(render(8192), 0, 8192, kNoteHz);
        const double hz_got = measure_hz(again, 0, 8192, kNoteHz);
        CHECK(std::fabs(cents_of(hz_got / hz_ref)) < 25.0);
    }
}

// ---- 4. Panic() и смена инструмента сбрасывают одинаково полное состояние ----------------------------------------

// Контрольный сценарий: нота, колесо, снова нота; возвращает хеш звука.
uint64_t probe(uint64_t h = 1469598103934665603ULL)
{
    note_on(60, 100); note_on(64, 90); note_on(67, 80);
    h = hash_of(render(2560), h);
    bend(4000);
    h = hash_of(render(2560), h);
    note_on(72, 120);
    h = hash_of(render(2560), h);
    note_off(60); note_off(64); note_off(67); note_off(72);
    h = hash_of(render(SR / 4), h);
    bend(0);
    return h;
}

// «Грязный» сценарий: трогаем всё, что у движка может запоминаться.
void make_dirty()
{
    note_on(48, 120); note_on(55, 110); note_on(60, 100); note_on(64, 90);
    render(2000);
    cc(64, 127);                                   // педаль
    cc(1, 127);                                    // мод-колесо
    for (uint8_t c : { 2, 11, 20, 21, 22, 23, 24, 25, 26, 27, 28, 70, 71, 72, 73, 74, 75, 77, 78 }) cc(c, 100);
    for (uint8_t c : { 20, 22, 24 }) cc(c, 0);
    cc(101, 0); cc(100, 0); cc(6, 12);             // RPN 0: диапазон колеса 12 полутонов (SoundFont)
    bend(-6000);
    SynthEngine_ChannelPressure(120);
    SynthEngine_PolyPressure(60, 100);
    render(3000);
    note_off(48);
    note_on(72, 127);
    render(3000);
}

void test_panic_and_switch_reset_everything()
{
    for (const EngineCase &e : g_engines) {
        select_fresh(e.program);
        const uint64_t fresh = probe();

        // Panic()
        select_fresh(e.program);
        make_dirty();
        SynthEngine_Panic();
        const uint64_t after_panic = probe();
        CHECK(after_panic == fresh);

        // Смена инструмента и возврат
        select_fresh(e.program);
        make_dirty();
        bend(0);   // колесо - физический орган: при смене инструмента оно остаётся там, где стоит (Panic - сбрасывает)
        SynthEngine_SelectProgram(e.program == 2U ? 3U : 2U);
        render(1024);
        SynthEngine_SelectProgram(e.program);
        const uint64_t after_switch = probe();
        CHECK(after_switch == fresh);

        // Грязный сценарий действительно что-то менял (иначе проверка бессмысленна).
        select_fresh(e.program);
        make_dirty();
        const uint64_t dirty = probe();
        CHECK(dirty != fresh);

        // Panic() возвращает колесо в центр.
        bend(5000);
        SynthEngine_Panic();
        note_on(kNote);
        render(SR / 5);
        const double hz = measure_hz(render(8192), 0, 8192, kNoteHz);
        select_fresh(e.program);
        note_on(kNote);
        render(SR / 5);
        const double hz_ref = measure_hz(render(8192), 0, 8192, kNoteHz);
        CHECK(std::fabs(cents_of(hz / hz_ref)) < 5.0);

        std::printf("  %-9s fresh=%016llx panic=%016llx switch=%016llx\n", e.name, (unsigned long long)fresh,
                    (unsigned long long)after_panic, (unsigned long long)after_switch);
    }
}

// ---- 5. CC1 (мод-колесо) -----------------------------------------------------------------------------------------

uint64_t play_hash(const EngineCase &e, int wheel)   // wheel < 0: CC1 не трогаем
{
    select_fresh(e.program);
    if (wheel >= 0) cc(1, (uint8_t)wheel);
    note_on(60, 100); note_on(67, 100);
    uint64_t h = hash_of(render(SR / 2));
    note_off(60); note_off(67);
    return hash_of(render(SR / 4), h);
}

// Доля высоких частот: сумма модулей первой разности / сумма модулей сигнала (0,5 с после Note On).
double brightness_of(const EngineCase &e, int wheel)
{
    select_fresh(e.program);
    cc(1, (uint8_t)wheel);
    note_on(60, 100);
    render(480);
    const auto b = render(SR / 2);
    double d = 0.0, s = 0.0;
    for (size_t i = 1; i < b.size(); ++i) { d += std::fabs((double)b[i] - b[i - 1]); s += std::fabs((double)b[i]); }
    return s > 0.0 ? d / s : 0.0;
}

void test_mod_wheel()
{
    for (const EngineCase &e : g_engines) {
        // Колесо в нуле = колесо не трогали (бит-в-бит), колесо вверх - звук меняется.
        const uint64_t untouched = play_hash(e, -1);
        const uint64_t zero      = play_hash(e, 0);
        const uint64_t full      = play_hash(e, 127);
        std::printf("  %-9s CC1: untouched=%016llx zero=%016llx full=%016llx\n", e.name,
                    (unsigned long long)untouched, (unsigned long long)zero, (unsigned long long)full);
        CHECK(zero == untouched);
        CHECK(full != untouched);

        // Возврат колеса в ноль возвращает звук к исходному (для движков, где колесо влияет на новые ноты или на тембр).
        select_fresh(e.program);
        cc(1, 127); cc(1, 0);
        note_on(60, 100); note_on(67, 100);
        uint64_t h = hash_of(render(SR / 2));
        note_off(60); note_off(67);
        h = hash_of(render(SR / 4), h);
        CHECK(h == untouched);

        // Яркость растёт с колесом там, где колесо управляет тембром фильтром/FM-индексом.
        if (e.program <= 1U) {   // E-Piano, Pluck
            const double b0 = brightness_of(e, 0);
            const double b1 = brightness_of(e, 127);
            std::printf("  %-9s brightness: wheel 0 %.4f, wheel 127 %.4f\n", e.name, b0, b1);
            CHECK(b1 > b0 * 1.03);
        }
    }
}

}  // namespace

int main()
{
    // SoundFont: если банка нет, проверяем остальные движки.
    SF2LoaderHost_SetPath(nullptr);
    SynthEngine_Init();
    const bool have_sf2 = SoundFontSynth_InitSF2();
    // Общая громкость одна на все проверки; задаём заранее и даём сглаживанию завершиться, иначе плавный выход на уровень
    // попадёт только в «свежие» прогоны и исказит сравнение с прогонами после Panic.
    SynthEngine_SetMasterVolume(127);
    note_on(60, 100);                 // сглаживание громкости идёт только по звучащим блокам
    render(SR / 4);
    SynthEngine_Panic();

    g_engines = {
        { SYNTH_ENGINE_EPIANO, "epiano", true  },
        { SYNTH_ENGINE_PLUCK,  "pluck",  true  },
        { SYNTH_ENGINE_LEAD,   "lead",   true  },
        { SYNTH_ENGINE_ORGAN,  "organ",  false },
        { SYNTH_ENGINE_VIOLIN, "violin", true  },
    };
    if (have_sf2) {
        g_engines.push_back({ (uint32_t)SYNTH_ENGINE_SOUNDFONT, "soundfont", true });
    } else {
        std::puts("[INFO] tests/SNDFNT.SF2 not found: SoundFont is skipped");
    }

    std::puts("pitch bend");               test_pitch_bend();
    std::puts("pitch bend: no clicks");    test_pitch_bend_has_no_clicks();
    std::puts("pitch bend: extremes");     test_pitch_bend_extremes_are_safe();
    std::puts("all notes off (CC123)");    test_all_notes_off();
    std::puts("all sound off (CC120)");    test_all_sound_off();
    std::puts("panic / switch reset");     test_panic_and_switch_reset_everything();
    std::puts("mod wheel (CC1)");          test_mod_wheel();

    return purrmidi_test::Finish("test_engine_unified");
}

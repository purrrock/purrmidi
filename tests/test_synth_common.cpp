// Тесты общей инфраструктуры движков: synth_common.h (NoteInput, SoftLimit, MidiFreq) и synth_arena.h.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "synth_arena.h"
#include "synth_common.h"
#include "check.h"

struct Ev { uint8_t type; uint8_t note; uint8_t aux; };
constexpr uint32_t kSize = 32, kLimit = 28;
using Input = synth::NoteInput<Ev, kSize, kLimit>;

static Input g_in;

static void test_basic_fifo_order() {
    g_in.Reset();
    for (uint8_t n = 0; n < 5; ++n) CHECK(g_in.NoteOn(n, Ev{0, n, 100}));
    Ev e{};
    for (uint8_t n = 0; n < 5; ++n) { CHECK(g_in.Pop(e)); CHECK(e.note == n); }
    CHECK(!g_in.Pop(e));
    CHECK(g_in.Dropped() == 0);
}

// Note On не может занять зарезервированные слоты: последние 4 слота - только для Note Off/CC64.
static void test_note_on_limit_and_critical_reserve() {
    g_in.Reset();
    int accepted = 0;
    for (int i = 0; i < 40; ++i) if (g_in.NoteOn(60, Ev{0, 60, 100})) ++accepted;
    CHECK(accepted == (int)kLimit);
    CHECK(g_in.Dropped() == 40 - kLimit);

    // Критичные события ещё помещаются в резерв (32 - 28 = 4 слота).
    for (int i = 0; i < (int)(kSize - kLimit); ++i) g_in.NoteOff(60, Ev{1, 60, 0});
    CHECK(g_in.RecoveryState() == 0);          // всё поместилось - восстановление не нужно
    g_in.NoteOff(60, Ev{1, 60, 0});            // 33-е событие: очередь заполнена на 100%
    CHECK(g_in.RecoveryState() == 1);          // запрошено аварийное восстановление
}

// Полный цикл восстановления: reset_engine вызван ровно один раз, очередь и счётчики очищены, приём возобновлён.
static void test_recovery_cycle() {
    g_in.Reset();
    for (int i = 0; i < 40; ++i) g_in.NoteOn(61, Ev{0, 61, 100});
    for (int i = 0; i < 6; ++i) g_in.NoteOff(61, Ev{1, 61, 0});       // переполнение -> REQUESTED
    CHECK(g_in.RecoveryState() == 1);

    // Пока идёт восстановление, новые события отклоняются.
    const uint32_t dropped_before = g_in.Dropped();
    CHECK(!g_in.NoteOn(62, Ev{0, 62, 100}));
    CHECK(g_in.Dropped() == dropped_before + 1);

    int resets = 0;
    g_in.ServiceRecovery([&]() { ++resets; });
    CHECK(resets == 1);
    CHECK(g_in.RecoveryState() == 0);

    Ev e{};
    CHECK(!g_in.Pop(e));                                              // очередь отброшена
    CHECK(g_in.NoteOn(62, Ev{0, 62, 100}));                           // приём снова работает
    CHECK(g_in.Pop(e) && e.note == 62);

    // Без запроса восстановление ничего не делает.
    g_in.ServiceRecovery([&]() { ++resets; });
    CHECK(resets == 1);
}

// Учёт нажатий: ReleasePress и откат при непринятом Note On не уходят ниже нуля и не ломают очередь.
static void test_press_accounting_is_safe() {
    g_in.Reset();
    g_in.ReleasePress(10);                     // ниже нуля не уходит
    g_in.NoteOff(10, Ev{1, 10, 0});
    Ev e{};
    CHECK(g_in.Pop(e) && e.type == 1);
    for (int i = 0; i < 300; ++i) g_in.NoteOn(11, Ev{0, 11, 100});    // насыщение на 255 не должно зациклиться
    CHECK(g_in.Dropped() >= 300 - kLimit);
}

static void test_soft_limit() {
    const float knee = 0.8f, ceil = 0.98f;
    CHECK(synth::SoftLimit(0.5f, knee, ceil) == 0.5f);                // ниже колена - без изменений
    CHECK(synth::SoftLimit(-0.8f, knee, ceil) == -0.8f);
    for (float x = 0.0f; x < 50.0f; x += 0.37f) {
        const float y = synth::SoftLimit(x, knee, ceil);
        CHECK(y <= ceil && y >= 0.0f);                                // не выше потолка
        CHECK(synth::SoftLimit(-x, knee, ceil) == -y);                // нечётная функция
    }
    CHECK(synth::SoftLimit(0.9f, knee, ceil) > synth::SoftLimit(0.85f, knee, ceil));   // монотонна
}

static void test_midi_freq() {
    CHECK(std::fabs(synth::MidiFreq(69) - 440.0f) < 1e-3f);           // A4
    CHECK(std::fabs(synth::MidiFreq(81) - 880.0f) < 1e-2f);           // A5
    CHECK(std::fabs(synth::MidiFreq(57) - 220.0f) < 1e-3f);           // A3
    CHECK(std::fabs(synth::MidiFreq(60) - 261.6256f) < 1e-2f);        // C4
    CHECK(synth::MidiFreq(200) == synth::MidiFreq(200 & 127));        // защита от ноты >= 128
}

// ---------------------------------------------------------------------------------------------------------------
// Арена
// ---------------------------------------------------------------------------------------------------------------
struct VoiceA { int a{7}; char pad[1000]; };
struct VoiceB { double b{3.5}; char pad[500]; };
static int g_owner_a, g_owner_b;

static void test_arena() {
    VoiceA *a = SynthArena_NewArray<VoiceA, 8>(&g_owner_a);
    CHECK(a != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(a) % 32 == 0);                  // выравнивание под строку кэша
    CHECK(SynthArena_IsOwner(&g_owner_a));
    CHECK(!SynthArena_IsOwner(&g_owner_b));
    for (int i = 0; i < 8; ++i) { CHECK(a[i].a == 7); CHECK(a[i].pad[999] == 0); }   // конструкторы выполнены
    a[3].a = 99; a[7].pad[500] = 55;

    VoiceB *b = SynthArena_NewArray<VoiceB, 4>(&g_owner_b);           // второй движок забирает арену
    CHECK(b != nullptr);
    CHECK(static_cast<void *>(a) == static_cast<void *>(b));          // та же память
    CHECK(!SynthArena_IsOwner(&g_owner_a));                           // прежний владелец потерял права
    CHECK(SynthArena_IsOwner(&g_owner_b));
    CHECK(SynthArena_Owner() == &g_owner_b);
    for (int i = 0; i < 4; ++i) CHECK(b[i].b == 3.5);                 // следов VoiceA нет

    // Повторный запрос тем же владельцем даёт чистую арену (так работает SynthEngine_Panic).
    VoiceB *b2 = SynthArena_NewArray<VoiceB, 4>(&g_owner_b);
    b[1].b = -1.0;
    b2 = SynthArena_NewArray<VoiceB, 4>(&g_owner_b);
    CHECK(b2[1].b == 3.5);

    CHECK(SynthArena_Acquire(&g_owner_a, SYNTH_ARENA_BYTES + 1) == nullptr);   // слишком много - отказ
    CHECK(SynthArena_Acquire(&g_owner_a, SYNTH_ARENA_BYTES) != nullptr);       // ровно весь размер - можно
}

// ---------------------------------------------------------------------------------------------------------------
// AutoGain: медленный регулятор усиления (Organ)
// ---------------------------------------------------------------------------------------------------------------
static void fill_sine(int16_t *buf, uint32_t n, double amp, double freq_hz, double &phase) {
    for (uint32_t i = 0; i < n; ++i) {
        buf[i] = (int16_t)std::lround(amp * 32767.0 * std::sin(phase));
        phase += 2.0 * 3.14159265358979323846 * freq_hz / 48000.0;
    }
}

static float block_peak(const float *x, uint32_t n) {
    float p = 0.0f;
    for (uint32_t i = 0; i < n; ++i) p = std::fmax(p, std::fabs(x[i]));
    return p;
}

// Пока пик * усиление не выше цели, регулятор прозрачен: выход равен обычному умножению на max_gain.
static void test_autogain_transparent_when_quiet() {
    synth::AutoGain ag(3.5f, 0.80f, 0.25f);
    int16_t in[128]; float out[128]; double ph = 0.0;
    for (int blk = 0; blk < 50; ++blk) {
        fill_sine(in, 128, 0.20, 440.0, ph);                    // 0,2 * 3,5 = 0,7 < 0,8
        ag.Process(in, out, 128);
        CHECK(ag.Gain() == 3.5f);
        for (int i = 0; i < 128; ++i) CHECK(std::fabs(out[i] - (float)in[i] * (3.5f / 32768.0f)) < 1e-6f);
    }
}

// Громкий устойчивый сигнал: пик держится у цели, а форма волны не искажается (отношение выход/вход постоянно в блоке).
static void test_autogain_limits_peak_without_distortion() {
    synth::AutoGain ag(3.5f, 0.80f, 0.25f);
    int16_t in[128]; float out[128]; double ph = 0.0;
    float worst_peak = 0.0f, best_peak = 10.0f, worst_ratio_spread = 0.0f;
    for (int blk = 0; blk < 400; ++blk) {
        fill_sine(in, 128, 0.50, 220.0, ph);                    // пик 0,5; нужное усиление 0,8 / 0,5 = 1,6
        ag.Process(in, out, 128);
        if (blk < 40) continue;                                  // первые блоки - переходный процесс
        const float p = block_peak(out, 128);
        if (p > worst_peak) worst_peak = p;
        if (p < best_peak && std::fabs((float)in[64]) > 1.0f) best_peak = std::fmax(p, 0.0f);
        float rmin = 1e9f, rmax = -1e9f;                         // отношение out/in на отсчётах с заметной амплитудой
        for (int i = 0; i < 128; ++i) {
            if (std::abs((int)in[i]) < 3000) continue;
            const float r = out[i] / ((float)in[i] / 32768.0f);
            rmin = std::fmin(rmin, r); rmax = std::fmax(rmax, r);
        }
        if (rmax > rmin && (rmax - rmin) / rmax > worst_ratio_spread) worst_ratio_spread = (rmax - rmin) / rmax;
    }
    CHECK(worst_peak <= 0.80f * 1.06f);                         // выше цели не поднимается (допуск на пульсацию)
    CHECK(worst_peak >= 0.80f * 0.85f);                         // и не занижает громкость сильнее допустимого
    CHECK(ag.Gain() > 1.50f && ag.Gain() < 1.70f);              // усиление около 1,6
    CHECK(worst_ratio_spread < 0.01f);                          // внутри блока усиление почти постоянно: форма волны цела
    (void)best_peak;
}

// Резкий громкий блок: усиление снижается плавно (линейно за kAttackRamp отсчётов), без скачка; затем медленно
// возвращается после затишья (постоянная времени 0,25 с).
static void test_autogain_attack_is_smooth_and_release_is_slow() {
    synth::AutoGain ag(3.5f, 0.80f, 0.25f);
    int16_t in[128]; float out[128]; double ph = 0.0;
    for (int blk = 0; blk < 20; ++blk) { fill_sine(in, 128, 0.05, 330.0, ph); ag.Process(in, out, 128); }
    CHECK(ag.Gain() == 3.5f);

    for (int i = 0; i < 128; ++i) in[i] = (int16_t)(i % 2 ? 29491 : -29491);   // пик 0,9: цель 0,8 / 0,9 = 0,889
    ag.Process(in, out, 128);
    float prev = 1e9f; bool monotone = true;
    for (int i = 0; i < (int)synth::AutoGain::kAttackRamp; ++i) {
        const float g = out[i] / ((float)in[i] / 32768.0f);
        if (g > prev + 1e-4f) monotone = false;
        prev = g;
    }
    CHECK(monotone);
    CHECK(out[0] / ((float)in[0] / 32768.0f) > 3.3f);           // начало блока ещё почти на старом усилении
    CHECK(std::fabs(out[127] / ((float)in[127] / 32768.0f) - 0.8f / 0.9f) < 0.01f);
    CHECK(std::fabs(ag.Gain() - 0.8f / 0.9f) < 0.01f);

    // Возврат: за одну постоянную времени (0,25 с = ~94 блока) усиление проходит около 63% пути к максимуму.
    const float g0 = ag.Gain();
    float last = g0; bool nondecreasing = true;
    for (int blk = 0; blk < 94; ++blk) {
        fill_sine(in, 128, 0.05, 330.0, ph); ag.Process(in, out, 128);
        if (ag.Gain() + 1e-6f < last) nondecreasing = false;
        last = ag.Gain();
    }
    CHECK(nondecreasing);
    const float expect = g0 + (3.5f - g0) * (1.0f - std::exp(-1.0f));
    CHECK(std::fabs(ag.Gain() - expect) < 0.15f);
    for (int blk = 0; blk < 1000; ++blk) { fill_sine(in, 128, 0.05, 330.0, ph); ag.Process(in, out, 128); }
    CHECK(ag.Gain() > 3.49f);

    ag.Reset();
    CHECK(ag.Gain() == 3.5f);
}

// ---------------------------------------------------------------------------------------------------------------
// LoadGovernor: регулятор нагрузки SoundFont (чистая логика, числа заданы вручную)
// ---------------------------------------------------------------------------------------------------------------
static void test_governor_learns_voice_cost_and_predicts() {
    synth::LoadGovernor g(85, 90);
    g.Reset();
    CHECK(g.CostPerVoiceX256() == 0U);
    CHECK(g.PredictRender(10, 128) == 0U);                       // пока не измерено - прогноза нет
    g.RenderDone(100000U, 10U, 128U);                            // 10 голосов, блок из 128 кадров: 10000 тактов на голос
    CHECK(g.PredictRender(10, 128) == 100000U);
    CHECK(g.PredictRender(5, 128) == 50000U);
    CHECK(g.PredictRender(10, 64) == 50000U);                    // стоимость пропорциональна числу кадров
    g.RenderDone(0U, 0U, 128U);                                  // нет голосов - в среднее не идёт
    CHECK(g.PredictRender(10, 128) == 100000U);
    for (int i = 0; i < 200; ++i) g.RenderDone(200000U, 10U, 128U);   // цена выросла вдвое: среднее сходится к новой
    CHECK(g.PredictRender(10, 128) > 195000U && g.PredictRender(10, 128) <= 200000U);

    CHECK(g.VoicesPerNote() == 4U);                              // до измерений предполагаем 4 региона на ноту
    for (int i = 0; i < 50; ++i) g.NoteStarted(2U);
    CHECK(g.VoicesPerNote() == 2U);
    g.NoteStarted(0U);                                           // нота не запустила голосов - не искажает среднее
    CHECK(g.VoicesPerNote() == 2U);
}

static void test_governor_reset_state_keeps_learned_costs() {
    synth::LoadGovernor g(85, 90);
    g.Reset();
    g.RenderDone(100000U, 10U, 128U);
    for (int i = 0; i < 50; ++i) g.NoteStarted(2U);
    (void)g.ShedSoundingNotes(95000U, 100000U);
    g.ResetState();                                          // выбор движка: оценки остаются
    CHECK(g.PredictRender(10, 128) == 100000U);
    CHECK(g.VoicesPerNote() == 2U);
    CHECK(g.ShedSoundingNotes(95000U, 100000U) == 0U);       // но счётчик перегрузки начат заново
    g.Reset();                                               // новый банк: оценки забываются
    CHECK(g.PredictRender(10, 128) == 0U);
    CHECK(g.VoicesPerNote() == 4U);
}

static void test_governor_defers_note_on_by_forecast() {
    synth::LoadGovernor g(85, 90);
    g.Reset();
    for (int i = 0; i < 100; ++i) g.RenderDone(100000U, 10U, 128U);    // 10000 тактов на голос
    for (int i = 0; i < 100; ++i) g.NoteStarted(4U);                   // 4 голоса на ноту
    const uint32_t budget = 200000U;                                   // лимит 85% = 170000

    // Первый Note On за блок не откладывается никогда, как бы ни было тяжело.
    CHECK(!g.ShouldDeferNoteOn(1000000U, 30U, 128U, budget, 0U));
    // Прогноз: elapsed + рендер (10 + 4 голосов) = 20000 + 140000 = 160000 <= 170000: применяем.
    CHECK(!g.ShouldDeferNoteOn(20000U, 10U, 128U, budget, 1U));
    // elapsed 40000: 40000 + 140000 = 180000 > 170000: откладываем.
    CHECK(g.ShouldDeferNoteOn(40000U, 10U, 128U, budget, 1U));
    // Голосов уже много: откладываем даже при нулевом elapsed.
    CHECK(g.ShouldDeferNoteOn(0U, 14U, 128U, budget, 1U));
    // Нет данных о цене голоса: прогноз только по elapsed.
    synth::LoadGovernor fresh(85, 90);
    fresh.Reset();
    CHECK(!fresh.ShouldDeferNoteOn(100000U, 30U, 128U, budget, 1U));
    CHECK(fresh.ShouldDeferNoteOn(180000U, 30U, 128U, budget, 1U));
}

static void test_governor_sheds_sounding_notes_only_when_overload_is_sustained() {
    synth::LoadGovernor g(85, 90);
    g.Reset();
    const uint32_t budget = 100000U;                              // порог гашения 90% = 90000, тяжёлая перегрузка > 110000
    // Одиночный всплеск не гасит ничего.
    CHECK(g.ShedSoundingNotes(95000U, budget) == 0U);
    CHECK(g.ShedSoundingNotes(50000U, budget) == 0U);             // нормальный блок сбрасывает счётчик
    CHECK(g.ShedSoundingNotes(95000U, budget) == 0U);
    // Второй блок подряд выше порога: гасим одну ноту.
    CHECK(g.ShedSoundingNotes(95000U, budget) == 1U);
    // Пауза между гашениями: пока ноты затухают (около 10 мс), новых не гасим.
    for (uint32_t i = 0; i + 1U < synth::LoadGovernor::kCooldownBlocks; ++i) CHECK(g.ShedSoundingNotes(95000U, budget) == 0U);
    CHECK(g.ShedSoundingNotes(95000U, budget) == 1U);             // пауза прошла, перегрузка держится
    // Тяжёлая перегрузка (более 110% бюджета): две ноты за раз.
    for (uint32_t i = 0; i + 1U < synth::LoadGovernor::kCooldownBlocks; ++i) (void)g.ShedSoundingNotes(120000U, budget);
    CHECK(g.ShedSoundingNotes(120000U, budget) == 2U);
    // Нагрузка спала: гашение прекращается.
    for (int i = 0; i < 10; ++i) CHECK(g.ShedSoundingNotes(50000U, budget) == 0U);
}

// Peek: посмотреть событие, не извлекая его.
static void test_peek_does_not_consume() {
    g_in.Reset();
    Ev e{};
    CHECK(!g_in.Peek(e));
    CHECK(g_in.NoteOn(10, Ev{0, 10, 100}));
    CHECK(g_in.NoteOn(11, Ev{0, 11, 100}));
    CHECK(g_in.Peek(e)); CHECK(e.note == 10);
    CHECK(g_in.Peek(e)); CHECK(e.note == 10);                   // повторный просмотр тот же
    CHECK(g_in.Pop(e));  CHECK(e.note == 10);
    CHECK(g_in.Peek(e)); CHECK(e.note == 11);
    CHECK(g_in.Pop(e));  CHECK(e.note == 11);
    CHECK(!g_in.Peek(e));
}

int main() {
    test_autogain_transparent_when_quiet();
    test_autogain_limits_peak_without_distortion();
    test_autogain_attack_is_smooth_and_release_is_slow();
    test_peek_does_not_consume();
    test_governor_learns_voice_cost_and_predicts();
    test_governor_reset_state_keeps_learned_costs();
    test_governor_defers_note_on_by_forecast();
    test_governor_sheds_sounding_notes_only_when_overload_is_sustained();
    test_basic_fifo_order();
    test_note_on_limit_and_critical_reserve();
    test_recovery_cycle();
    test_press_accounting_is_safe();
    test_soft_limit();
    test_midi_freq();
    test_arena();

    if (purrmidi_test::Failures() != 0) {
        std::fprintf(stderr, "test_synth_common: %d check(s) failed\n", purrmidi_test::Failures());
        return 1;
    }
    std::puts("test_synth_common: OK");
    return 0;
}

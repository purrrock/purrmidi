// Тесты общей инфраструктуры движков: synth_common.h (NoteInput, SoftLimit, MidiFreq) и synth_arena.h.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "synth_arena.h"
#include "synth_common.h"

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

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

int main() {
    test_basic_fifo_order();
    test_note_on_limit_and_critical_reserve();
    test_recovery_cycle();
    test_press_accounting_is_safe();
    test_soft_limit();
    test_midi_freq();
    test_arena();

    if (g_failures != 0) {
        std::fprintf(stderr, "test_synth_common: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("test_synth_common: OK");
    return 0;
}

// Сквозной тест всех движков через публичный API: фиксированный MIDI-сценарий (аккорд, педаль, CC, перегрузка очереди
// событий с аварийным восстановлением) рендерится в буфер, по выходу считается хеш.
//
//  * Обычный запуск проверяет СВОЙСТВА: каждый движок звучит, и повторный проход по тем же движкам (с переключениями
//    между ними, т.е. с повторным использованием общей арены памяти голосов) даёт бит-в-бит тот же результат, что и
//    первый: после переключения в памяти не остаётся «следов» другого движка.
//  * Режим --hash печатает хеш каждого движка. Сравнение хешей ДО и ПОСЛЕ рефакторинга (в одной сборке/на одном
//    компиляторе) доказывает, что звук не изменился. Абсолютные значения между компиляторами/libm не переносимы,
//    поэтому в репозиторий они не зашиты.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "midi_dispatch.h"
#include "synth_engine.h"
#include "check.h"

namespace {

constexpr uint32_t kFrames = 128;

struct Hasher {
    uint64_t h = 1469598103934665603ULL;   // FNV-1a 64
    void add(const int16_t *p, size_t n) {
        const uint8_t *b = reinterpret_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n * sizeof(int16_t); ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    }
};

void pc(uint8_t program)               { MIDI_Event_t e = {MIDI_STATUS_PROGRAM_CHANGE, program, 0}; MIDI_Dispatch(&e); }
void note_on(uint8_t n, uint8_t v)     { MIDI_Event_t e = {MIDI_STATUS_NOTE_ON, n, v};  MIDI_Dispatch(&e); }
void note_off(uint8_t n)               { MIDI_Event_t e = {MIDI_STATUS_NOTE_OFF, n, 0}; MIDI_Dispatch(&e); }
void cc(uint8_t c, uint8_t v)          { MIDI_Event_t e = {MIDI_STATUS_CONTROL_CHANGE, c, v}; MIDI_Dispatch(&e); }

struct Result { uint64_t hash; int peak; };

void render(Hasher &hs, int &peak, int blocks) {
    std::vector<int16_t> buf(kFrames * 2);
    for (int b = 0; b < blocks; ++b) {
        SynthEngine_FillStereoBuffer(buf.data(), kFrames);
        hs.add(buf.data(), buf.size());
        for (int16_t s : buf) { int a = s < 0 ? -(int)s : (int)s; if (a > peak) peak = a; }
    }
}

// Один и тот же сценарий для любого движка.
Result run_script(uint8_t program) {
    Hasher hs; int peak = 0;
    std::srand(12345);       // Pluck (DaisySP) берёт шум возбуждения из rand(): без этого прогоны не воспроизводимы
    pc(program);

    note_on(60, 100); note_on(64, 90); note_on(67, 80);
    render(hs, peak, 10);

    cc(64, 127); note_off(60);                       // педаль + отпускание
    render(hs, peak, 5);

    cc(1, 90); cc(71, 40); cc(72, 100); cc(74, 80);  // популярные CC
    note_on(72, 127);
    render(hs, peak, 10);

    cc(64, 0); note_off(64); note_off(67); note_off(72);
    render(hs, peak, 20);                            // хвосты

    // Перегрузка очереди: много Note On подряд без рендера (часть отбрасывается), затем лавина Note Off.
    for (int i = 0; i < 60; ++i) note_on((uint8_t)(36 + (i % 48)), (uint8_t)(60 + (i % 60)));
    render(hs, peak, 5);
    for (int i = 0; i < 80; ++i) note_off((uint8_t)(36 + (i % 48)));
    cc(64, 127); cc(64, 0);
    render(hs, peak, 20);                            // аварийное восстановление

    note_on(60, 100);                                // после восстановления движок обязан работать
    render(hs, peak, 10);
    note_off(60);
    render(hs, peak, 30);
    return {hs.h, peak};
}

}  // namespace

int main(int argc, char **argv) {
    const bool print_hash = (argc > 1 && std::strcmp(argv[1], "--hash") == 0);

    // Все движки без SoundFont (ему нужен банк; он проверяется отдельными тестами).
    struct Entry { uint8_t program; const char *name; };
    const Entry engines[] = {
        {0, "epiano"}, {1, "pluck"}, {2, "lead"}, {3, "organ"}, {4, "dulcimer"}, {5, "flute"}, {6, "saxophone"},
    };

    SynthEngine_Init();

    // Проход 1: в «неудобном» порядке, чтобы соседние движки были с разными размерами памяти.
    const int order1[] = {4, 2, 5, 0, 6, 1, 3};
    // Проход 2: другой порядок, те же сценарии.
    const int order2[] = {1, 3, 6, 4, 0, 2, 5};

    Result first[7], second[7];
    for (int k : order1) { first[k]  = run_script(engines[k].program); }
    for (int k : order2) { second[k] = run_script(engines[k].program); }

    for (int k = 0; k < 7; ++k) {
        if (print_hash) std::printf("engine=%-9s hash=%016llx peak=%d\n", engines[k].name,
                                    (unsigned long long)first[k].hash, first[k].peak);
        if (first[k].hash != second[k].hash)
            std::fprintf(stderr, "  mismatch in %s: %016llx vs %016llx (peak %d vs %d)\n", engines[k].name,
                         (unsigned long long)first[k].hash, (unsigned long long)second[k].hash,
                         first[k].peak, second[k].peak);
        CHECK(first[k].peak > 0);                       // движок звучит
        CHECK(first[k].hash == second[k].hash);         // после переключений звук тот же бит-в-бит
        CHECK(first[k].peak == second[k].peak);
    }

    if (purrmidi_test::Failures() != 0) {
        std::fprintf(stderr, "test_engine_golden: %d check(s) failed\n", purrmidi_test::Failures());
        return 1;
    }
    if (!print_hash) std::puts("test_engine_golden: OK");
    return 0;
}

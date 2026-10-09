// Тест рантайм-переключения синтезаторов по MIDI Program Change.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "midi_dispatch.h"
#include "synth_engine.h"

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond    \
                      << std::endl;                                              \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static void send_pc(uint8_t program, uint8_t channel = 0) {
    MIDI_Event_t e = {static_cast<uint8_t>(MIDI_STATUS_PROGRAM_CHANGE | channel), program, 0};
    MIDI_Dispatch(&e);
}

static void send_note_on(uint8_t note, uint8_t vel) {
    MIDI_Event_t e = {MIDI_STATUS_NOTE_ON, note, vel};
    MIDI_Dispatch(&e);
}

static void send_note_off(uint8_t note) {
    MIDI_Event_t e = {MIDI_STATUS_NOTE_OFF, note, 0};
    MIDI_Dispatch(&e);
}

// Возвращает максимум |sample| в блоке из num_frames стереофреймов.
static int render_peak(uint32_t num_frames) {
    std::vector<int16_t> buf(static_cast<size_t>(num_frames) * 2, 0x5555);  // не ноль: проверяем, что буфер перезаписан
    SynthEngine_FillStereoBuffer(buf.data(), num_frames);
    int peak = 0;
    for (int16_t s : buf) {
        int a = s < 0 ? -static_cast<int>(s) : static_cast<int>(s);
        if (a > peak) peak = a;
    }
    return peak;
}

static void test_default_is_epiano() {
    SynthEngine_Init();
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_EPIANO);
    CHECK(SYNTH_ENGINE_DEFAULT == SYNTH_ENGINE_EPIANO);
    CHECK(std::strcmp(SynthEngine_GetName(), "epiano") == 0);

    // Без нот — тишина (и буфер действительно перезаписан)
    CHECK(render_peak(512) == 0);

    send_note_on(60, 100);
    CHECK(render_peak(4800) > 0);
}

static void test_program_change_selects_engine() {
    SynthEngine_Init();

    send_pc(1);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_PLUCK);
    CHECK(std::strcmp(SynthEngine_GetName(), "pluck") == 0);

    send_pc(2);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SINE);
    CHECK(std::strcmp(SynthEngine_GetName(), "sine") == 0);

    send_pc(0);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_EPIANO);

    // Канал сообщения не важен (omni)
    send_pc(2, 9);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SINE);
    send_pc(1, 15);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_PLUCK);
}

static void test_program_wraps_modulo() {
    SynthEngine_Init();
    send_pc(3);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_EPIANO);
    send_pc(4);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_PLUCK);
    send_pc(5);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SINE);
    send_pc(127);   // 127 % 3 == 1
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_PLUCK);
}

static void test_every_engine_makes_sound() {
    for (uint8_t prog = 0; prog < SYNTH_ENGINE_COUNT; ++prog) {
        SynthEngine_Init();
        send_pc(prog);
        CHECK(static_cast<int>(SynthEngine_GetCurrent()) == prog);
        CHECK(render_peak(512) == 0);            // сразу после смены — тишина
        send_note_on(64, 110);
        CHECK(render_peak(4800) > 0);            // нота звучит
        send_note_off(64);
        render_peak(48000 * 2);                  // даём затухнуть
    }
}

static void test_same_program_does_not_cut_note() {
    SynthEngine_Init();
    send_note_on(60, 100);
    CHECK(render_peak(2400) > 0);

    send_pc(0);                                  // E-Piano уже активен
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_EPIANO);
    CHECK(render_peak(480) > 0);                 // нота продолжает звучать
}

static void test_switch_silences_old_engine() {
    SynthEngine_Init();
    send_note_on(60, 100);
    CHECK(render_peak(2400) > 0);

    send_pc(2);                                  // sine: нот нет -> тишина
    CHECK(render_peak(4800) == 0);

    send_pc(0);                                  // возврат: старая нота не должна «воскреснуть»
    CHECK(render_peak(4800) == 0);

    send_note_on(60, 100);
    CHECK(render_peak(4800) > 0);                // а новые ноты работают
}

static void test_select_api() {
    SynthEngine_Init();
    CHECK(SynthEngine_Select(SYNTH_ENGINE_SINE) == 1);
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SINE);
    CHECK(SynthEngine_Select(SYNTH_ENGINE_COUNT) == 0);   // вне диапазона
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_SINE); // не изменился
    CHECK(std::strcmp(SynthEngine_GetEngineName(SYNTH_ENGINE_EPIANO), "epiano") == 0);
    CHECK(std::strcmp(SynthEngine_GetEngineName(SYNTH_ENGINE_COUNT), "?") == 0);

    SynthEngine_Init();                                    // Init возвращает E-Piano
    CHECK(SynthEngine_GetCurrent() == SYNTH_ENGINE_EPIANO);
}

// Аудио-поток крутит FillStereoBuffer, пока главный поток шлёт ноты и Program Change.
// Проверка отсутствия падений / гонок (имеет смысл запускать и под ThreadSanitizer).
static void test_switch_while_audio_running() {
    SynthEngine_Init();
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> blocks{0};

    std::thread audio([&]() {
        std::vector<int16_t> buf(128 * 2);
        while (!stop.load()) {
            SynthEngine_FillStereoBuffer(buf.data(), 128);
            blocks.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    uint8_t prog = 0;
    while (std::chrono::steady_clock::now() < end) {
        send_note_on(static_cast<uint8_t>(48 + (prog % 24)), 100);
        send_pc(prog++);
        send_note_off(static_cast<uint8_t>(48 + ((prog + 5) % 24)));
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }

    stop.store(true);
    audio.join();
    CHECK(blocks.load() > 0);
    CHECK(static_cast<int>(SynthEngine_GetCurrent()) < SYNTH_ENGINE_COUNT);
}

int main() {
    test_default_is_epiano();
    test_program_change_selects_engine();
    test_program_wraps_modulo();
    test_every_engine_makes_sound();
    test_same_program_does_not_cut_note();
    test_switch_silences_old_engine();
    test_select_api();
    test_switch_while_audio_running();

    if (g_failures != 0) {
        std::cerr << "test_synth_switch: " << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_synth_switch passed successfully." << std::endl;
    return 0;
}

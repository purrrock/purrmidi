#include "organ_synth.h"
#include "synth_common.h"
#include "synth_organ.hpp"

#include <atomic>
#include <cstring>

namespace {

enum EventType : uint8_t { EV_NOTE_ON = 0, EV_NOTE_OFF = 1, EV_CC = 2 };

struct Event {
    uint8_t type;
    uint8_t a;  /* note / controller */
    uint8_t b;  /* velocity / value  */
};

constexpr uint32_t ORGAN_EVENT_FIFO_SIZE = 64;
constexpr uint32_t ORGAN_MAX_EVENTS_PER_BLOCK = 16;

/* Выравнивание громкости с остальными движками. Библиотека bfreeOrgan2 рассчитана с большим запасом по уровню
 * (на 12 нотах с выдвинутыми регистрами пик около -18 дБFS, на аккорде из трёх нот с типовыми регистрами - около -12 дБFS),
 * из-за чего орган звучал на 8-10 дБ тише E-Piano/Lead/Violin. Поэтому здесь добавляется усиление, а чтобы плотные
 * аккорды не клиппировали, сигнал проходит через тот же мягкий ограничитель, что и в Lead/Violin. Ниже колена
 * (0,80 от полной шкалы) сигнал не искажается. */
constexpr float ORGAN_OUTPUT_GAIN  = 3.5f;      /* +10,9 дБ */
constexpr float ORGAN_LIMIT_KNEE   = 0.80f;
constexpr float ORGAN_LIMIT_CEIL   = 0.98f;

inline int16_t organ_level(int16_t s)
{
    const float x = synth::SoftLimit((float)s * (ORGAN_OUTPUT_GAIN / 32768.0f), ORGAN_LIMIT_KNEE, ORGAN_LIMIT_CEIL);
    return (int16_t)(x * 32767.0f);
}

Event                 g_fifo[ORGAN_EVENT_FIFO_SIZE];
std::atomic<uint32_t> g_fifo_head{0};
std::atomic<uint32_t> g_fifo_tail{0};
std::atomic<uint32_t> g_dropped_events{0};
std::atomic<bool>     g_overflow_occurred{false};

synth::synth_organ    g_organ;

bool fifo_push(const Event& ev)
{
    uint32_t head = g_fifo_head.load(std::memory_order_relaxed);
    uint32_t tail = g_fifo_tail.load(std::memory_order_acquire);

    if (head - tail >= ORGAN_EVENT_FIFO_SIZE) {
        g_dropped_events.fetch_add(1, std::memory_order_relaxed);
        g_overflow_occurred.store(true, std::memory_order_release);
        return false;
    }
    g_fifo[head & (ORGAN_EVENT_FIFO_SIZE - 1)] = ev;
    g_fifo_head.store(head + 1, std::memory_order_release);
    return true;
}

bool fifo_pop(Event& ev)
{
    uint32_t tail = g_fifo_tail.load(std::memory_order_relaxed);
    uint32_t head = g_fifo_head.load(std::memory_order_acquire);

    if (head == tail) {
        return false;
    }
    ev = g_fifo[tail & (ORGAN_EVENT_FIFO_SIZE - 1)];
    g_fifo_tail.store(tail + 1, std::memory_order_release);
    return true;
}

void process_event(const Event& ev)
{
    midi::command_t cmd{};
    cmd.channel = 0;

    switch (ev.type) {
        case EV_NOTE_ON:
            cmd.status = midi::status_t::NOTE_ON;
            cmd.data   = ev.a;
            cmd.value  = ev.b;
            g_organ.push_midi_cmd(cmd);
            break;

        case EV_NOTE_OFF:
            cmd.status = midi::status_t::NOTE_OFF;
            cmd.data   = ev.a;
            cmd.value  = 0;
            g_organ.push_midi_cmd(cmd);
            break;

        case EV_CC:
            if (ev.a == 120) {
                // All Sound Off: immediate silence
                g_organ.reset_all();
            } else if (ev.a == 123) {
                // All Notes Off: notes go through their normal release (no click)
                g_organ.deactivate_all();
            } else {
                cmd.status = midi::status_t::CONTROLLER_CHANGE;
                cmd.data   = ev.a;
                cmd.value  = ev.b;
                g_organ.push_midi_cmd(cmd);
            }
            break;

        default:
            break;
    }
}

void process_events()
{
    if (g_overflow_occurred.exchange(false, std::memory_order_acquire)) {
        // Queue overflow: note events may have been lost, so silence all voices to avoid
        // stuck notes. Stale NOTE events are discarded, but queued controller changes
        // (drawbars, chorus, envelope) are still applied so the panel state is not lost.
        g_organ.reset_all();
        Event stale;
        for (uint32_t i = 0; i < ORGAN_EVENT_FIFO_SIZE && fifo_pop(stale); ++i) {
            if (stale.type == EV_CC) {
                process_event(stale);
            }
        }
        return;
    }

    uint32_t processed = 0;
    Event ev;
    while (processed < ORGAN_MAX_EVENTS_PER_BLOCK && fifo_pop(ev)) {
        process_event(ev);
        processed++;
    }
}

} // namespace

extern "C" {

void OrganSynth_Init(void)
{
    g_organ.init();
    g_fifo_head.store(0, std::memory_order_relaxed);
    g_fifo_tail.store(0, std::memory_order_relaxed);
    g_dropped_events.store(0, std::memory_order_relaxed);
    g_overflow_occurred.store(false, std::memory_order_relaxed);
}

uint32_t OrganSynth_GetDroppedEventCount(void)
{
    return g_dropped_events.load(std::memory_order_relaxed);
}

void OrganSynth_NoteOn(uint8_t midi_note, uint8_t velocity)
{
    if (velocity == 0) {
        OrganSynth_NoteOff(midi_note);
        return;
    }
    if (midi_note > 127) {
        return;
    }
    Event ev = { EV_NOTE_ON, midi_note, (uint8_t)(velocity > 127 ? 127 : velocity) };
    fifo_push(ev);
}

void OrganSynth_NoteOff(uint8_t midi_note)
{
    if (midi_note > 127) {
        return;
    }
    Event ev = { EV_NOTE_OFF, midi_note, 0 };
    fifo_push(ev);
}

void OrganSynth_ControlChange(uint8_t control, uint8_t value)
{
    Event ev = { EV_CC, control, (uint8_t)(value > 127 ? 127 : value) };
    fifo_push(ev);
}

void OrganSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames)
{
    process_events();

    /* Render the mono block, then duplicate it into the stereo buffer. */
    constexpr uint32_t kChunk = 128;
    static int16_t mono[kChunk];
    uint32_t done = 0;
    while (done < num_frames) {
        const uint32_t n = (num_frames - done > kChunk) ? kChunk : (num_frames - done);
        g_organ.synthesize_block(mono, n);
        for (uint32_t i = 0; i < n; ++i) {
            const int16_t s = organ_level(mono[i]);
            buffer[(done + i) * 2]     = s;
            buffer[(done + i) * 2 + 1] = s;
        }
        done += n;
    }
}

} // extern "C"

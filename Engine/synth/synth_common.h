/**
 * @file    synth_common.h
 * @brief   Общая инфраструктура звуковых движков: очередь событий с протоколом аварийного восстановления,
 *          мягкий ограничитель, таблица частот MIDI-нот.
 *
 * Раньше эти куски были скопированы в каждый движок (Flute, Saxophone, Dulcimer, Lead, Pluck): FIFO событий
 * совпадало до символа, протокол восстановления и производители NoteOn/NoteOff/CC64 - с точностью до имён.
 * Здесь они собраны в один проверенный тестами (test_synth_common) компонент. Поведение движков не изменилось:
 * звук до и после выноса совпадает бит-в-бит (test_engine_golden).
 *
 * Поток данных:
 *   main  --NoteOn/NoteOff/Critical-->  SPSC-FIFO  --Pop()-->  аудио-контекст (PendSV / аудио-поток)
 * Один производитель (main) и один потребитель (рендер): блокировок нет, только атомарные счётчики.
 */
#ifndef SYNTH_COMMON_H
#define SYNTH_COMMON_H

#include <atomic>
#include <cmath>
#include <cstdint>

namespace synth {

/* ------------------------------------------------------------------------------------------------------------
 * Мягкий ограничитель (tanh-колено): до `knee` сигнал не меняется, выше плавно прижимается к `ceil` (< 1.0),
 * так что клиппинга при переводе в int16 не бывает.
 * ------------------------------------------------------------------------------------------------------------ */
inline float SoftLimit(float x, float knee, float ceil)
{
    const float a = std::abs(x);
    if (a <= knee) return x;
    const float room = ceil - knee;
    const float y = knee + room * std::tanh((a - knee) / room);
    return (x < 0.0f) ? -y : y;
}

/* ------------------------------------------------------------------------------------------------------------
 * Частота MIDI-ноты в герцах (равномерно темперированный строй, A4 = 440 Гц). Таблица на 128 нот считается один раз
 * (при первом обращении или при MidiFreqInit() из Init() движка) и общая для всех движков.
 * ------------------------------------------------------------------------------------------------------------ */
inline float g_midi_freq[128];
inline bool  g_midi_freq_ready = false;

inline void MidiFreqInit()
{
    if (g_midi_freq_ready) return;
    for (int i = 0; i < 128; i++) {
        g_midi_freq[i] = 440.0f * std::pow(2.0f, (float)(i - 69) / 12.0f);
    }
    g_midi_freq_ready = true;
}

inline float MidiFreq(uint8_t note)
{
    MidiFreqInit();
    return g_midi_freq[note & 127U];
}

/* ------------------------------------------------------------------------------------------------------------
 * NoteInput: очередь событий движка + учёт нажатий + протокол аварийного восстановления.
 *
 *  Event       - структура события движка (копируется в FIFO целиком).
 *  kSize       - ёмкость FIFO (степень двойки).
 *  kNoteOnLimit - сколько слотов доступно обычным Note On; остальные зарезервированы под критичные события
 *                 (Note Off, педаль CC64), чтобы лавина нот не вытеснила их и не оставила «залипших» нот.
 *
 * Протокол восстановления (если критичное событие всё же не поместилось - FIFO заполнено на 100%):
 *   1. производитель выставляет состояние REQUESTED;
 *   2. потребитель (ServiceRecovery) переводит его в IN_PROGRESS и ждёт, пока все производители, уже вошедшие в
 *      NoteOn/NoteOff/Critical, выйдут (active_producer_ops == 0);
 *   3. вызывает reset_engine() (движок гасит все свои голоса и педаль), обнуляет счётчики нажатий, отбрасывает
 *      содержимое FIFO и возвращается в NORMAL. Пока состояние не NORMAL, новые события отклоняются.
 * ------------------------------------------------------------------------------------------------------------ */
template <typename Event, uint32_t kSize, uint32_t kNoteOnLimit>
class NoteInput {
    static_assert(kSize != 0U && (kSize & (kSize - 1U)) == 0U, "FIFO size must be a power of two");
    static_assert(kNoteOnLimit <= kSize, "Note On limit must not exceed the FIFO size");

public:
    /* ---------------- сторона производителя (main) ---------------- */

    /** Поставить Note On. note < 128 проверяет вызывающий. Возвращает true, если событие принято в очередь. */
    bool NoteOn(uint8_t note, const Event &ev)
    {
        /* Протокол входа: регистрируем операцию (seq_cst) и только потом смотрим состояние восстановления. */
        active_producer_ops_.fetch_add(1, std::memory_order_seq_cst);
        if (recovery_state_.load(std::memory_order_seq_cst) != kRecoveryNormal) {
            active_producer_ops_.fetch_sub(1, std::memory_order_seq_cst);
            dropped_note_on_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        /* Счётчик нажатий (насыщение на 255) увеличиваем заранее; если событие не влезло - откатываем. */
        uint8_t count = press_count_[note].load(std::memory_order_relaxed);
        while (count < 255) {
            if (press_count_[note].compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) break;
        }

        const bool queued = PushNoteOn(ev);
        if (!queued) {
            uint8_t cur = press_count_[note].load(std::memory_order_relaxed);
            while (cur > 0) {
                if (press_count_[note].compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed)) break;
            }
        }

        active_producer_ops_.fetch_sub(1, std::memory_order_seq_cst);
        return queued;
    }

    /** Поставить Note Off (критичное событие: использует всю ёмкость FIFO; при полном переполнении - восстановление). */
    void NoteOff(uint8_t note, const Event &ev)
    {
        active_producer_ops_.fetch_add(1, std::memory_order_seq_cst);
        if (recovery_state_.load(std::memory_order_seq_cst) != kRecoveryNormal) {
            active_producer_ops_.fetch_sub(1, std::memory_order_seq_cst);
            return;
        }

        uint8_t count = press_count_[note].load(std::memory_order_relaxed);
        while (count > 0) {
            if (press_count_[note].compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) break;
        }

        if (!PushCritical(ev)) {
            recovery_state_.store(kRecoveryRequested, std::memory_order_seq_cst);
        }
        active_producer_ops_.fetch_sub(1, std::memory_order_seq_cst);
    }

    /** Поставить иное критичное событие (педаль CC64) без учёта нажатий. */
    void Critical(const Event &ev)
    {
        active_producer_ops_.fetch_add(1, std::memory_order_seq_cst);
        if (recovery_state_.load(std::memory_order_seq_cst) != kRecoveryNormal) {
            active_producer_ops_.fetch_sub(1, std::memory_order_seq_cst);
            return;
        }
        if (!PushCritical(ev)) {
            recovery_state_.store(kRecoveryRequested, std::memory_order_seq_cst);
        }
        active_producer_ops_.fetch_sub(1, std::memory_order_seq_cst);
    }

    /* ---------------- сторона потребителя (рендер) ---------------- */

    bool Pop(Event &ev)
    {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if (head == tail) return false;
        ev = fifo_[tail & (kSize - 1U)];
        tail_.store(tail + 1U, std::memory_order_release);
        return true;
    }

    /**
     * Потребитель «украл» голос, который держал ноту `note` (вытеснение при нехватке голосов): снимаем с неё одно
     * нажатие из учёта, чтобы счётчик не остался завышенным. Насыщение на 0.
     */
    void ReleasePress(uint8_t note)
    {
        uint8_t cur = press_count_[note & 127U].load(std::memory_order_relaxed);
        while (cur > 0) {
            if (press_count_[note & 127U].compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed)) break;
        }
    }

    /**
     * Шаг протокола восстановления (вызывается рендером перед разбором событий). Если запрошено восстановление и
     * производителей в критических секциях нет, вызывает reset_engine() - движок сбрасывает свои голоса и педаль, -
     * а затем сбрасывает счётчики нажатий и очередь.
     */
    template <typename ResetFn>
    void ServiceRecovery(ResetFn &&reset_engine)
    {
        if (recovery_state_.load(std::memory_order_seq_cst) == kRecoveryNormal) return;

        recovery_state_.store(kRecoveryInProgress, std::memory_order_seq_cst);
        if (active_producer_ops_.load(std::memory_order_seq_cst) == 0U) {
            reset_engine();
            for (int i = 0; i < 128; i++) {
                press_count_[i].store(0, std::memory_order_relaxed);
            }
            const uint32_t head = head_.load(std::memory_order_relaxed);
            tail_.store(head, std::memory_order_release);
            recovery_state_.store(kRecoveryNormal, std::memory_order_seq_cst);
        }
    }

    /* ---------------- общее ---------------- */

    /** Полный сброс (из Init() движка, пока никто не работает с очередью). */
    void Reset()
    {
        for (int i = 0; i < 128; i++) {
            press_count_[i].store(0, std::memory_order_relaxed);
        }
        active_producer_ops_.store(0, std::memory_order_relaxed);
        recovery_state_.store(kRecoveryNormal, std::memory_order_relaxed);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        dropped_note_on_.store(0, std::memory_order_relaxed);
    }

    /** Сколько Note On отброшено (очередь переполнена или шло восстановление). */
    uint32_t Dropped() const { return dropped_note_on_.load(std::memory_order_relaxed); }

    /** Для тестов: состояние протокола восстановления (0 - норма). */
    uint32_t RecoveryState() const { return recovery_state_.load(std::memory_order_relaxed); }

private:
    enum : uint32_t { kRecoveryNormal = 0, kRecoveryRequested = 1, kRecoveryInProgress = 2 };

    bool PushNoteOn(const Event &ev)
    {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= kNoteOnLimit) {
            dropped_note_on_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        fifo_[head & (kSize - 1U)] = ev;
        head_.store(head + 1U, std::memory_order_release);
        return true;
    }

    bool PushCritical(const Event &ev)
    {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= kSize) return false;
        fifo_[head & (kSize - 1U)] = ev;
        head_.store(head + 1U, std::memory_order_release);
        return true;
    }

    Event fifo_[kSize];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    std::atomic<uint32_t> dropped_note_on_{0};
    std::atomic<uint8_t>  press_count_[128];
    std::atomic<uint32_t> recovery_state_{kRecoveryNormal};
    std::atomic<uint32_t> active_producer_ops_{0};
};

}  // namespace synth

#endif /* SYNTH_COMMON_H */

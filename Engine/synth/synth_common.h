/**
 * @file    synth_common.h
 * @brief   Общая инфраструктура звуковых движков: очередь событий с протоколом аварийного восстановления,
 *          мягкий ограничитель, таблица частот MIDI-нот.
 *
 * Раньше эти куски были скопированы в каждый движок (Violin, Lead, Pluck): FIFO событий
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

#if defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64)
#include <xmmintrin.h>
#endif

namespace synth {

/* ------------------------------------------------------------------------------------------------------------
 * Сброс денормалей в ноль (flush-to-zero). Затухающие хвосты резонаторов, фильтров и огибающих экспоненциально
 * приближаются к нулю и переходят в денормальные числа; обработка денормалей в FPU в десятки раз медленнее обычных
 * операций (на хосте измерено: 3,6 мс против 0,09 мс на блок у резонатора Elements). На Cortex-M7 это бит FZ регистра FPSCR,
 * на x86 - FTZ/DAZ в MXCSR. Вызывается в начале каждого блока рендера: состояние FPU лежит в контексте
 * обработчика (PendSV) или аудио-потока, поэтому повтор безвреден и стоит пару тактов.
 * ------------------------------------------------------------------------------------------------------------ */
inline void EnableFlushToZero()
{
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t fpscr;
    __asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1UL << 24);                                  /* FPSCR.FZ */
    __asm volatile("vmsr fpscr, %0" : : "r"(fpscr));
#elif defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64)
    _mm_setcsr(_mm_getcsr() | 0x8040U);                    /* FTZ (бит 15) + DAZ (бит 6) */
#endif
}

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
 * Медленный регулятор усиления (AutoGain): держит пик блока не выше `target_peak`, не искажая форму волны.
 *
 * Зачем. SoftLimit - статическая кривая: пока пик выходит за колено, она сплющивает вершины каждого периода, то есть
 * вносит гармонические искажения («хрип»). Для движка, у которого усиление выбрано под одну-три ноты (Organ: x3,5), на
 * плотных аккордах этого не избежать. Здесь усиление меняется медленно и одинаково для всех отсчётов блока, поэтому
 * форма волны сохраняется, а громкость аккорда просто не растёт выше цели.
 *
 * Как работает. Для каждого блока (блок уже отрисован, его пик известен заранее, предсказание не нужно):
 *   - цель = min(max_gain, target_peak / пик_блока);
 *   - если цель ниже текущего усиления (аккорд стал громче): усиление линейно доходит до цели за kAttackRamp
 *     отсчётов (быстро, но без щелчка); остаток перерегулирования, если он есть, снимет SoftLimit;
 *   - иначе усиление медленно (с постоянной времени release_seconds) растёт к цели: за время затухания аккорда нет
 *     «накачки», а на низких нотах (период до 30 мс) нет модуляции усиления с частотой сигнала.
 * Пока пик * max_gain <= target_peak (например, 1-3 ноты органа), усиление равно max_gain, и выход бит-в-бит
 * совпадает с обычным умножением на max_gain.
 * ------------------------------------------------------------------------------------------------------------ */
class AutoGain {
public:
    static constexpr uint32_t kAttackRamp = 32U;   /* отсчётов на снижение усиления (около 0,7 мс при 48 кГц) */

    AutoGain(float max_gain, float target_peak, float release_seconds, float sample_rate = 48000.0f)
        : max_gain_(max_gain), target_peak_(target_peak), release_seconds_(release_seconds),
          sample_rate_(sample_rate), gain_(max_gain) {}

    /** Вернуть усиление к максимальному (при Init() движка). */
    void Reset() { gain_ = max_gain_; }

    /** Текущее усиление (после последнего блока). */
    float Gain() const { return gain_; }

    /**
     * Обработать блок: in - int16-отсчёты движка, out - float, 1.0 = полная шкала, уже умноженные на усиление.
     * Дальше обычно идёт SoftLimit как страховка.
     */
    void Process(const int16_t *in, float *out, uint32_t n)
    {
        int32_t peak_i = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const int32_t a = (in[i] < 0) ? -(int32_t)in[i] : (int32_t)in[i];
            if (a > peak_i) peak_i = a;
        }
        const float peak = (float)peak_i * (1.0f / 32768.0f);
        const float target = (peak * max_gain_ > target_peak_) ? (target_peak_ / peak) : max_gain_;

        const float g0 = gain_;
        float goal;
        uint32_t ramp;
        if (target < g0) {
            goal = target;
            ramp = (n < kAttackRamp) ? n : kAttackRamp;
        } else {
            const float k = 1.0f - std::exp(-(float)n / (sample_rate_ * release_seconds_));
            goal = g0 + (target - g0) * k;
            ramp = n;
        }

        const float step = (ramp != 0U) ? (goal - g0) / (float)ramp : 0.0f;
        float g = g0;
        for (uint32_t i = 0; i < n; ++i) {
            if (i < ramp) g += step; else g = goal;
            out[i] = (float)in[i] * (1.0f / 32768.0f) * g;
        }
        gain_ = goal;
    }

private:
    float max_gain_;
    float target_peak_;
    float release_seconds_;
    float sample_rate_;
    float gain_;
};

/* ------------------------------------------------------------------------------------------------------------
 * Регулятор нагрузки движка с дорогими голосами (SoundFont): чистая логика без обращений к железу, поэтому проверяется
 * хост-тестами с заданными числами (test_synth_common). Всё считается в тактах процессора; блок - это один вызов
 * рендера, его бюджет `block_budget` передаёт движок.
 *
 * Две задачи:
 *  1) Не растягивать блок событиями. Старт голоса в TinySoundFont дорог (десятки pow/tan/log в double), а аккорд
 *     из пяти клавиш запускает десятки голосов в одном блоке, и блок вылетает за бюджет (хрип, пропуск блока).
 *     ShouldDeferNoteOn() говорит, что очередной Note On лучше отложить на следующий блок: прогноз = уже потраченное на
 *     события + оценка рендера всех голосов, включая будущие голоса этой ноты, по измеренной цене голоса.
 *     Хотя бы один Note On за блок применяется всегда, поэтому ноты не могут застрять.
 *  2) Если рендер устойчиво (не один блок) выходит за порог, а отпущенных голосов для гашения уже нет, нужно погасить
 *     звучащие ноты: ShedSoundingNotes() возвращает, сколько нот гасить сейчас (0, 1 или 2), с паузой между
 *     гашениями, потому что затухание идёт ~10 мс (4 блока) и голос ещё стоит рендеру тактов.
 * Цена голоса - скользящее среднее (1/8) тактов рендера на голос на кадр, в формате с фиксированной точкой x256.
 * ------------------------------------------------------------------------------------------------------------ */
class LoadGovernor {
public:
    static constexpr uint32_t kCooldownBlocks  = 4U;    /* пауза между гашениями звучащих нот */
    static constexpr uint32_t kOverBlocksToShed = 2U;   /* столько блоков подряд выше порога, прежде чем гасить */

    LoadGovernor(uint32_t event_limit_pct, uint32_t shed_pct)
        : event_limit_pct_(event_limit_pct), shed_pct_(shed_pct) {}

    /** Полный сброс, включая выученные оценки (новый банк: цена голоса и число регионов на ноту могли измениться). */
    void Reset()
    {
        cost_x256_ = 0U;
        voices_per_note_x16_ = 4U * 16U;
        ResetState();
    }

    /**
     * Сброс только счётчиков перегрузки (выбор движка, All Sound Off). Выученные оценки сохраняются: цена голоса - свойство
     * банка и железа, и забывать её при каждом переключении нельзя: первый же аккорд после выбора SoundFont шёл бы без
     * прогноза и мог вылететь за бюджет.
     */
    void ResetState()
    {
        over_blocks_ = 0U;
        cooldown_ = 0U;
    }

    /** После рендера: обновить цену голоса. voices - сколько голосов рендерилось в этом блоке. */
    void RenderDone(uint32_t render_cycles, uint32_t voices, uint32_t frames)
    {
        if (voices == 0U || frames == 0U) return;
        const uint32_t sample = (uint32_t)(((uint64_t)render_cycles * 256U) / ((uint64_t)voices * frames));
        if (cost_x256_ == 0U) {
            cost_x256_ = sample;
        } else if (sample >= cost_x256_) {
            cost_x256_ += (sample - cost_x256_) / 8U;
        } else {
            cost_x256_ -= (cost_x256_ - sample) / 8U;
        }
    }

    /** После Note On: сколько голосов он запустил (регионов банка на ноту); 0 - не запустил, в среднее не идёт. */
    void NoteStarted(uint32_t voices_started)
    {
        if (voices_started == 0U) return;
        const uint32_t sample = voices_started * 16U;
        /* Шаг округляется вверх: иначе среднее на целых числах застревает рядом с целью и не доходит до неё. */
        if (sample >= voices_per_note_x16_) {
            voices_per_note_x16_ += (sample - voices_per_note_x16_ + 3U) / 4U;
        } else {
            voices_per_note_x16_ -= (voices_per_note_x16_ - sample + 3U) / 4U;
        }
    }

    /** Ожидаемое число голосов одной ноты (округление вверх). */
    uint32_t VoicesPerNote() const { return (voices_per_note_x16_ + 15U) / 16U; }

    /** Цена голоса, такты на голос на кадр, x256 (0 - ещё не измерена). */
    uint32_t CostPerVoiceX256() const { return cost_x256_; }

    /** Оценка времени рендера для `voices` голосов и `frames` кадров. */
    uint32_t PredictRender(uint32_t voices, uint32_t frames) const
    {
        return (uint32_t)(((uint64_t)cost_x256_ * voices * frames) / 256U);
    }

    /**
     * Отложить ли очередной Note On на следующий блок.
     * elapsed - такты, уже потраченные на события в этом блоке; voices_now - голосов сейчас;
     * applied_note_ons - сколько Note On уже применено в этом блоке (первый не откладывается никогда).
     */
    bool ShouldDeferNoteOn(uint32_t elapsed, uint32_t voices_now, uint32_t frames, uint32_t block_budget,
                           uint32_t applied_note_ons) const
    {
        if (applied_note_ons == 0U) return false;
        const uint64_t limit = (uint64_t)block_budget * event_limit_pct_ / 100U;
        const uint64_t predicted = (uint64_t)elapsed + PredictRender(voices_now + VoicesPerNote(), frames);
        return predicted > limit;
    }

    /**
     * После рендера: сколько звучащих нот погасить сейчас. Вызывается на каждом блоке (ведёт счётчики).
     * 0 - ничего; 1 - устойчивая перегрузка (порог shed_pct); 2 - тяжёлая (выше 110% бюджета).
     */
    uint32_t ShedSoundingNotes(uint32_t render_cycles, uint32_t block_budget)
    {
        if (cooldown_ != 0U) --cooldown_;
        const uint64_t limit = (uint64_t)block_budget * shed_pct_ / 100U;
        if ((uint64_t)render_cycles <= limit) {
            over_blocks_ = 0U;
            return 0U;
        }
        if (over_blocks_ < 255U) ++over_blocks_;
        if (over_blocks_ < kOverBlocksToShed || cooldown_ != 0U) return 0U;
        cooldown_ = kCooldownBlocks;
        return ((uint64_t)render_cycles * 100U > (uint64_t)block_budget * 110U) ? 2U : 1U;
    }

private:
    uint32_t event_limit_pct_;
    uint32_t shed_pct_;
    uint32_t cost_x256_ = 0U;
    uint32_t voices_per_note_x16_ = 4U * 16U;
    uint32_t over_blocks_ = 0U;
    uint32_t cooldown_ = 0U;
};

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
 *  kNoteOnLimit - сколько слотов доступно обычным событиям (Note On, Soft); остальные зарезервированы под критичные
 *                 (Note Off, педаль CC64, All Notes Off), чтобы лавина нот не вытеснила их и не оставила «залипших»
 *                 нот.
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

    /**
     * Поставить некритичное событие (непрерывный контроллер, Program Change): оно занимает только «обычную» часть
     * FIFO (kNoteOnLimit), как Note On, и при нехватке места просто отбрасывается (счётчик Dropped). Восстановление
     * оно никогда не запускает и резерв под Note Off не расходует. Возвращает true, если событие принято.
     */
    bool Soft(const Event &ev)
    {
        active_producer_ops_.fetch_add(1, std::memory_order_seq_cst);
        if (recovery_state_.load(std::memory_order_seq_cst) != kRecoveryNormal) {
            active_producer_ops_.fetch_sub(1, std::memory_order_seq_cst);
            dropped_note_on_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const bool queued = PushNoteOn(ev);
        active_producer_ops_.fetch_sub(1, std::memory_order_seq_cst);
        return queued;
    }

    /** Поставить иное критичное событие (педаль CC64, All Notes Off) без учёта нажатий. */
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

    /** Посмотреть следующее событие, не извлекая его (только потребитель). Нужен, чтобы отложить событие до следующего блока. */
    bool Peek(Event &ev) const
    {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if (head == tail) return false;
        ev = fifo_[tail & (kSize - 1U)];
        return true;
    }

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

    /** Сколько некритичных событий (Note On, Soft) отброшено: очередь переполнена или шло восстановление. */
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

#ifndef SYNTH_REVERB_H
#define SYNTH_REVERB_H

/*
 * Общая реверберация (CC91): один эффект поверх выхода любого движка.
 *
 * Схема: FDN (сеть задержек с обратной связью) из 8 линий взаимно простых длин (13-37 мс) с матрицей смешивания
 * Хаусхолдера, затуханием по времени и потерями высоких частот в каждой линии (однополюсный ФНЧ), на входе две
 * цепочки Шрёдер-allpass для диффузии. Вход моно ((L+R)/2), выход стерео: правый и левый каналы снимаются
 * с линий по разным ортогональным знаковым наборам, поэтому хвост широкий и не коррелирует между каналами.
 *
 * Свойства, на которые опирается остальной код (все проверяются тестами test_reverb и test_engine_reverb):
 *   - при нулевой глубине выход бит-в-бит равен входу, а процессорное время не тратится;
 *   - любое изменение глубины, включение и выключение идут линейной рампой внутри блока (без щелчков);
 *   - Kill() (All Sound Off) гасит хвост за ~21 мс и обнуляет состояние;
 *   - сумма сухого и мокрого сигнала мягко ограничивается, в int16 клиппинга нет;
 *   - состояние (около 38 КБ float) лежит в обычной статической памяти: на STM32 это DTCM, быстрая, без кэша и
 *     без ECC-хлопот. Ничего не сохраняется между включениями: глубина стартует со значения по умолчанию.
 *
 * Потоки: Init(), Reset() и Process() вызываются только из аудио-контекста (или из main, пока аудио держит замок
 * SynthEngine). Kill() можно вызывать из main в любой момент: он только выставляет атомарный флаг.
 */

#include "synth_common.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace synth {

class Reverb {
public:
    static constexpr uint32_t kLines = 8U;

    /* Длины линий, отсчёты при 48 кГц (простые числа). Сумма 9300 = 38 КБ в float вместе с диффузорами. */
    static constexpr uint32_t kLineLen[kLines] = {601U, 743U, 887U, 1049U, 1223U, 1399U, 1597U, 1801U};
    static constexpr uint32_t kAp1Len = 149U;
    static constexpr uint32_t kAp2Len = 113U;

    /* Время затухания на низких частотах (RT60, с) и потери высоких: с ними на 4 кГц хвост короче примерно вдвое. */
    static constexpr float kRt60Seconds = 1.8f;
    static constexpr float kDamping     = 0.35f;
    static constexpr float kApGain      = 0.6f;

    /* Нормировка выхода: мокрый сигнал на широкополосном шуме по уровню равен входу (подобрано тестом test_reverb). */
    static constexpr float kOutScale = 0.98f;

    /* Мягкий предел суммы (доля полной шкалы): ниже колена сигнал не меняется. */
    static constexpr float kLimitKnee    = 0.92f;
    static constexpr float kLimitCeiling = 0.999f;

    /* Длительность гашения хвоста при Kill(), отсчётов (около 21 мс). */
    static constexpr uint32_t kKillSamples = 1024U;

    /* После гашения вход ещё столько отсчётов (около 170 мс) не подаётся в эффект. Движки гасят голоса за 10-20 мс, но
     * Pluck (физическая модель) доигрывает остаток до ~110 мс, и без паузы он породил бы новый, слабый, но долгий хвост.
     * Цена: новая нота в первые ~190 мс после All Sound Off звучит без реверберации (CC120 - редкая «паника»). */
    static constexpr uint32_t kKillHoldSamples = 8192U;

    /** Полный сброс: коэффициенты затухания линий, состояние и рампа глубины (после него поведение как у только что включённого эффекта). */
    void Init()
    {
        for (uint32_t i = 0; i < kLines; ++i) {
            line_gain_[i] = std::pow(10.0f, -3.0f * (float)kLineLen[i] / (48000.0f * kRt60Seconds));
        }
        gain_ = 0.0f;
        kill_req_.store(0U, std::memory_order_relaxed);
        killing_ = false;
        Reset();
    }

    /** Обнуляет линии и фильтры (мгновенная тишина). Рампы глубины не трогает. */
    void Reset()
    {
        std::memset(mem_, 0, sizeof(mem_));
        std::memset(ap1_, 0, sizeof(ap1_));
        std::memset(ap2_, 0, sizeof(ap2_));
        std::memset(pos_, 0, sizeof(pos_));
        std::memset(lp_, 0, sizeof(lp_));
        ap1_pos_ = 0U;
        ap2_pos_ = 0U;
        dirty_   = false;
    }

    /**
     * All Sound Off: вход эффекта сразу заглушается, за kKillSamples отсчётов мокрый сигнал уходит в ноль, затем состояние
     * обнуляется, а ещё kKillHoldSamples вход остаётся заглушённым. Из любого контекста.
     */
    void Kill() { kill_req_.store(1U, std::memory_order_release); }

    /** Идёт ли обработка (глубина или хвост не нулевые): для диагностики и тестов. */
    bool IsActive() const { return dirty_ || gain_ > 0.0f || killing_; }

    /** Текущее применённое усиление мокрого сигнала (конец прошлого блока). */
    float AppliedWet() const { return gain_; }

    /**
     * Обработать блок на месте: io - стерео с чередованием (L, R, ...), int16; wet_target - усиление мокрого сигнала
     * к концу блока (0 - выкл.). Между блоками усиление меняется линейно.
     */
    void Process(int16_t *io, uint32_t frames, float wet_target)
    {
        if (kill_req_.exchange(0U, std::memory_order_acq_rel) != 0U) {
            killing_  = true;
            kill_pos_ = 0U;
        }

        if (wet_target <= 0.0f && gain_ <= 0.0f && !killing_) {
            if (dirty_) {
                Reset();   /* глубина ушла в ноль: хвост не должен «воскреснуть» при следующем включении */
            }
            return;
        }
        if (frames == 0U) {
            return;
        }
        dirty_ = true;

        const float step = (wet_target - gain_) / (float)frames;
        float       g    = gain_;
        for (uint32_t i = 0; i < frames; ++i) {
            g += step;
            const float l = (float)io[2U * i];
            const float r = (float)io[2U * i + 1U];

            if (killing_) {
                /* Вход заглушён. Пока идёт гашение (kill_pos_ < kKillSamples), доигрываем хвост с падающей громкостью. */
                float wl = 0.0f, wr = 0.0f;
                if (kill_pos_ < kKillSamples) {
                    Tick(0.0f, wl, wr);
                    const float k = g * (1.0f - (float)kill_pos_ * (1.0f / (float)kKillSamples));
                    wl *= k;
                    wr *= k;
                }
                ++kill_pos_;
                if (kill_pos_ == kKillSamples) {
                    Reset();
                    dirty_ = true;   /* Reset() сбросил признак, но обработка в этом блоке продолжается */
                } else if (kill_pos_ >= kKillSamples + kKillHoldSamples) {
                    killing_ = false;
                }
                io[2U * i]      = Out(l + wl);
                io[2U * i + 1U] = Out(r + wr);
                continue;
            }

            float wl, wr;
            Tick(0.5f * (l + r), wl, wr);
            io[2U * i]      = Out(l + wl * g);
            io[2U * i + 1U] = Out(r + wr * g);
        }
        gain_ = wet_target;
    }

private:
    static constexpr uint32_t kTotal = kLineLen[0] + kLineLen[1] + kLineLen[2] + kLineLen[3] +
                                       kLineLen[4] + kLineLen[5] + kLineLen[6] + kLineLen[7];

    /* Знаки входа и выходов линий (разные наборы: хвост стерео, входной сигнал не только в одной моде). */
    static constexpr float kInSign[kLines] = {1.f, -1.f, 1.f, 1.f, -1.f, 1.f, -1.f, -1.f};
    static constexpr float kSignL[kLines]  = {1.f, -1.f, 1.f, -1.f, 1.f, -1.f, 1.f, -1.f};
    static constexpr float kSignR[kLines]  = {1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, -1.f};

    static int16_t Out(float v)
    {
        const float knee = kLimitKnee * 32768.0f;
        const float a    = v < 0.0f ? -v : v;
        if (a > knee) {
            v = SoftLimit(v * (1.0f / 32768.0f), kLimitKnee, kLimitCeiling) * 32768.0f;
        }
        if (v > 32767.0f)  return 32767;
        if (v < -32768.0f) return -32768;
        return (int16_t)v;
    }

    /* Один отсчёт: вход моно, выход стерео (мокрый сигнал, без усиления рампы). */
    void Tick(float in, float &out_l, float &out_r)
    {
        /* диффузия на входе: два allpass подряд */
        float x;
        {
            float *b = ap1_;
            const float d = b[ap1_pos_];
            const float v = in - kApGain * d;
            b[ap1_pos_]   = v;
            if (++ap1_pos_ >= kAp1Len) ap1_pos_ = 0U;
            x = d + kApGain * v;
        }
        {
            float *b = ap2_;
            const float d = b[ap2_pos_];
            const float v = x - kApGain * d;
            b[ap2_pos_]   = v;
            if (++ap2_pos_ >= kAp2Len) ap2_pos_ = 0U;
            x = d + kApGain * v;
        }

        float o[kLines];
        float sum = 0.0f;
        uint32_t off = 0U;
        for (uint32_t i = 0; i < kLines; ++i) {
            const float raw = mem_[off + pos_[i]];
            lp_[i] += (1.0f - kDamping) * (raw - lp_[i]);
            o[i] = lp_[i] * line_gain_[i];
            sum += o[i];
            off += kLineLen[i];
        }

        const float k = sum * (2.0f / (float)kLines);   /* Хаусхолдер: y = o - (2/N) * sum(o) */
        float l = 0.0f, r = 0.0f;
        off = 0U;
        for (uint32_t i = 0; i < kLines; ++i) {
            mem_[off + pos_[i]] = (o[i] - k) + kInSign[i] * x * 0.5f;
            if (++pos_[i] >= kLineLen[i]) pos_[i] = 0U;
            off += kLineLen[i];
            l += kSignL[i] * o[i];
            r += kSignR[i] * o[i];
        }
        out_l = l * kOutScale;
        out_r = r * kOutScale;
    }

    float                 mem_[kTotal];
    float                 ap1_[kAp1Len];
    float                 ap2_[kAp2Len];
    float                 lp_[kLines];
    float                 line_gain_[kLines];
    uint32_t              pos_[kLines];
    uint32_t              ap1_pos_;
    uint32_t              ap2_pos_;
    float                 gain_;
    uint32_t              kill_pos_;
    bool                  killing_;
    bool                  dirty_;
    std::atomic<uint32_t> kill_req_;
};

/* ------------------------------------------------------------------------------------------------------------
 * ReverbLoadGuard: когда времени на блок не хватает, реверберация уходит первой, раньше, чем начнут гаситься голоса.
 * Смотрит на время рендера самого движка (без реверберации, чтобы эффект не раскачивал решение сам себя):
 *   - выше shed_pct бюджета блока `shed_blocks` блоков подряд: эффект отключается (плавно, внутри блока);
 *   - вернуть его можно после `restore_blocks` подряд блоков ниже restore_pct (пауза гистерезиса).
 * Бюджет 0 (нет часов, например на ПК) - эффект всегда разрешён.
 * ------------------------------------------------------------------------------------------------------------ */
class ReverbLoadGuard {
public:
    ReverbLoadGuard(uint32_t shed_pct, uint32_t restore_pct, uint32_t shed_blocks, uint32_t restore_blocks)
        : shed_pct_(shed_pct), restore_pct_(restore_pct), shed_blocks_(shed_blocks), restore_blocks_(restore_blocks) {}

    void Reset()
    {
        shed_      = false;
        over_      = 0U;
        calm_      = 0U;
    }

    /** После рендера движка: сколько тактов он занял и каков бюджет блока. Возвращает, разрешена ли реверберация. */
    bool Update(uint32_t engine_cycles, uint32_t budget_cycles)
    {
        if (budget_cycles == 0U) {
            Reset();
            return true;
        }
        const uint64_t used100 = (uint64_t)engine_cycles * 100U;
        if (!shed_) {
            if (used100 > (uint64_t)budget_cycles * shed_pct_) {
                if (++over_ >= shed_blocks_) {
                    shed_ = true;
                    calm_ = 0U;
                }
            } else {
                over_ = 0U;
            }
        } else {
            if (used100 < (uint64_t)budget_cycles * restore_pct_) {
                if (++calm_ >= restore_blocks_) {
                    shed_ = false;
                    over_ = 0U;
                }
            } else {
                calm_ = 0U;
            }
        }
        return !shed_;
    }

    bool IsShed() const { return shed_; }

private:
    uint32_t shed_pct_, restore_pct_, shed_blocks_, restore_blocks_;
    bool     shed_  = false;
    uint32_t over_  = 0U;
    uint32_t calm_  = 0U;
};

} // namespace synth

#endif /* SYNTH_REVERB_H */

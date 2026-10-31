/**
 * @file    violin_synth.cpp
 * @brief   Синтезатор скрипки: волновод смычковой струны + банк резонансов деки.
 *
 * Схема одного голоса (всё считается на частоте дискретизации 48 кГц):
 *
 *                 подставка                      смычок                       порожек
 *   дека  <--- [ветвь «подставка»] <---+--- нелинейное трение ---+---> [ветвь «гриф»] ---> (-1)
 *                      |               |                        |               |
 *                      +-- фильтр НЧ --+<----------------------->+<--- (-1) -----+
 *                          (потери)
 *
 * Скорость струны под смычком = отражённая от подставки + отражённая от порожка волны. Разность между скоростью
 * смычка и скоростью струны подаётся в таблицу трения: пока смычок «липнет» к струне, таблица близка к 1 и смычок
 * вводит энергию, при проскальзывании — падает к нулю (движение Гельмгольца, пилообразная волна).
 * Результат трения добавляется в обе ветви. Основная частота определяется суммарной задержкой петли:
 *      D = L_подставка + L_гриф + задержка фильтра потерь = fs / f0.
 *
 * Дробная часть длины хранится в ветви «гриф» (фазочастотно-плоский all-pass), ветвь «подставка» имеет целую длину:
 * так все потери интерполяции собраны в одном месте, а вибрато и Pitch Bend меняют только ветвь «гриф».
 *
 * Потокобезопасность: события из main приходят через FIFO (synth_common.h), непрерывные параметры — через
 * атомарные float/int; весь рендер выполняется в одном (аудио) контексте.
 */

#include "violin_synth.h"
#include "synth_arena.h"
#include "synth_common.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

/* ------------------------------------------------------------------------------------------------------------
 * Константы
 * ------------------------------------------------------------------------------------------------------------ */
constexpr float    kSampleRate = 48000.0f;
constexpr float    kTwoPi      = 6.28318530718f;
constexpr int      kVoices     = VIOLIN_VOICES;

constexpr uint32_t kNeckSize   = 1024;                 /* ветвь «гриф»: степень двойки, хватает до ~41 Гц */
constexpr uint32_t kNeckMask   = kNeckSize - 1U;
constexpr uint32_t kBridgeSize = 256;                  /* ветвь «подставка»: до 0,22 длины струны */
constexpr uint32_t kBridgeMask = kBridgeSize - 1U;

constexpr uint32_t kBlock      = 32;                   /* подблок: параметры обновляются раз в 32 кадра (0,67 мс) */
constexpr uint32_t kFifoSize   = 64;                   /* очередь событий (степень двойки) */
constexpr uint32_t kNoteOnLimit = 56;                  /* остальные слоты зарезервированы под Note Off и педаль */

constexpr int      kMinNote    = 31;                   /* G1 - ниже струна не помещается в линию задержки */
constexpr int      kMaxNote    = 108;                  /* C8 - выше петля становится короче 12 отсчётов */

constexpr int32_t  kFadeSamples = 128;                 /* гашение вытесняемого голоса: 2,7 мс */

/* Положение смычка (доля длины струны со стороны подставки): CC70 = 0 ... 127 -> 0.07 ... 0.22; по умолчанию 0.127. */
constexpr float    kBowPosMin     = 0.07f;
constexpr float    kBowPosRange   = 0.15f;

/* Потери в петле. Пока смычок на струне, потери ЗА ПЕРИОД постоянны для всех нот (так выдерживается окно устойчивого
 * движения Гельмгольца по всей клавиатуре: при потерях «за секунду» низкие ноты теряли за период больше и
 * срывались в хаотичный режим). После отпускания смычка струна гасится с постоянной времени в секундах. */
constexpr float    kSustainGain   = 0.9985f;           /* усиление петли за период, смычок на струне */
constexpr float    kPressVelK     = 1.0f;               /* прижим растёт со скоростью смычка (окно Шеллинга) */
constexpr float    kReleaseBase   = 0.12f;             /* время отпускания смычка при CC72 по умолчанию, с */
constexpr float    kRingReleaseK  = 0.8f;              /* затухание струны после отпускания = kRingReleaseK * время отпускания */

/* Вибрато: частота, максимальный размах (±кент) и задержка включения после начала ноты. */
constexpr float    kVibratoHz     = 5.4f;
constexpr float    kVibratoBase    = 0.35f;            /* глубина вибрато по умолчанию (мод-колесо в нуле) */
constexpr float    kKillTime       = 0.020f;           /* All Sound Off: время спада, с */
constexpr float    kVibratoMaxFrac = 0.0163f;          /* ≈ ±28 центов: 2^(28/1200) - 1 */
constexpr float    kVibratoDelay  = 0.18f;             /* до начала вибрато, с */
constexpr float    kVibratoRise   = 0.35f;             /* нарастание до полной глубины, с */

/* Блокиратор постоянной составляющей в петле струны: ФВЧ первого порядка. Без него постоянная составляющая от
 * смычка накапливается в петле (две инверсии, потери почти нулевые) и сдвигает рабочую точку трения: низкие ноты
 * «разгонялись» по секунде и перескакивали между режимами. Частота среза мала в сравнении с низшей нотой (49 Гц),
 * а на строй он не влияет (измерено: сдвиг < 1 цента на всём диапазоне; компенсация фазы не нужна - нелинейная петля
 * синхронизируется по групповой, а не фазовой задержке, и эти два эффекта у ФВЧ взаимно гасятся). */
constexpr float    kDcBlockHz     = 12.0f;
constexpr float    kDcBlockR      = 1.0f - kTwoPi * kDcBlockHz / kSampleRate;

/* Поправка строя на высоких нотах: доля длины петли на 1 Гц (до 1100 Гц). */
constexpr float    kPitchTrim     = 5.2e-6f;

/* Pitch Bend: ±2 полутона. */
constexpr float    kBendSemitones = 2.0f;

/* Общий уровень (по измерениям: одиночная нота при velocity 100 даёт пик ≈ 0.5 FS). */
constexpr float    kOutGain       = 0.90f;

/* ------------------------------------------------------------------------------------------------------------
 * События и параметры
 * ------------------------------------------------------------------------------------------------------------ */
enum EventType : uint8_t { EV_NOTE_ON, EV_NOTE_OFF, EV_SUSTAIN, EV_KILL };

struct ViolinEvent {
    EventType type;
    uint8_t   note;    /* номер ноты (Note On / Off) */
    uint8_t   value;   /* значение педали */
    float     vel;     /* velocity 0..1 */
};

/* Параметры, меняющиеся с MIDI (main пишет, аудио читает). */
std::atomic<float>   g_vibrato{0.0f};       /* CC1 (мод-колесо) 0..1: прибавка вибрато к kVibratoBase, в нуле звук по умолчанию */
std::atomic<float>   g_expression{1.0f};    /* CC2/CC11: экспрессия 0..1 */
std::atomic<float>   g_bow_pos{0.38f};      /* CC70: положение смычка 0..1 (0.38 -> β ≈ 0.127) */
std::atomic<float>   g_pressure{0.55f};     /* CC71: прижим 0..1 */
std::atomic<float>   g_release{0.5f};       /* CC72: время отпускания 0..1 (0.5 -> 1.0x) */
std::atomic<float>   g_attack{0.5f};        /* CC73: время атаки 0..1 (0.5 -> 1.0x) */
std::atomic<float>   g_bright{0.55f};       /* CC74: яркость 0..1 */
std::atomic<float>   g_noise{0.4f};         /* CC75: шум смычка 0..1 */
std::atomic<int32_t> g_bend{0};             /* Pitch Bend -8192..8191 */
std::atomic<float>   g_aftertouch{0.0f};    /* Channel Pressure 0..1 */

/* Очередь событий и учёт нажатий (общий компонент). */
synth::NoteInput<ViolinEvent, kFifoSize, kNoteOnLimit> g_input;

/* ------------------------------------------------------------------------------------------------------------
 * Голос
 * ------------------------------------------------------------------------------------------------------------ */
struct PendingNote {
    uint8_t note;
    float   vel;
};

struct Voice {
    float    neck[kNeckSize];          /* линия задержки «гриф» */
    float    bridge[kBridgeSize];      /* линия задержки «подставка» */
    uint32_t neck_w;                   /* индексы записи (растут без ограничения, маскируются при доступе) */
    uint32_t bridge_w;
    uint32_t bridge_len;               /* целая длина ветви «подставка» (фиксируется на время ноты) */
    float    ap_y;                     /* состояние all-pass интерполятора ветви «гриф» */
    float    lp_y;                     /* состояние фильтра потерь на подставке */
    float    dc_x1;                    /* состояние блокиратора постоянной составляющей в петле */
    float    dc_y1;

    float    total_len;                /* D0 = fs / f0 для ноты без Pitch Bend, отсчётов */
    float    env;                      /* огибающая скорости смычка 0..1 */
    float    att_coef;                 /* коэффициент разгона смычка за отсчёт (зависит от velocity и CC73) */
    float    bow_peak;                 /* пиковая скорость смычка для этой ноты */
    float    vel;                      /* velocity 0..1 */
    float    pitch_gain;               /* выравнивание громкости по высоте (низкие ноты иначе громче) */
    float    loop_gain;                /* усиление петли за период (сглаживается) */
    float    level;                    /* огрублённый уровень сигнала (для освобождения голоса) */
    float    burst;                    /* всплеск шума при атаке, затухает за ~20 мс */
    uint32_t age_samples;              /* сколько отсчётов звучит нота (для задержки вибрато) */
    uint32_t order;                    /* порядковый номер запуска (для вытеснения самого старого) */

    int32_t  fade_left;                /* >0: голос гасится перед стартом pending-ноты */
    PendingNote pending;

    std::atomic<int32_t> note_state{-1};   /* -1 свободен, 0..127 — нота, которую держит голос */
    uint8_t  press_count;              /* сколько раз нажата нота (Note On без Note Off) */
    bool     gate;                     /* смычок на струне */
    bool     kill;                     /* All Sound Off: быстрый спад независимо от клавиши и педали */
    bool     sustain_held;             /* нота удерживается педалью */
};

/* Голоса лежат в общей арене памяти (synth_arena.h). Указатель устанавливается в Init(); если арену забрал другой
 * движок, SynthArena_IsOwner(&g_voices) == false. */
Voice *g_voices = nullptr;

uint32_t g_order_counter = 0;          /* счётчик запусков нот */
bool     g_pedal         = false;      /* состояние педали (только аудио-контекст) */

/* ------------------------------------------------------------------------------------------------------------
 * Общее для всех голосов: шум, вибрато, дека
 * ------------------------------------------------------------------------------------------------------------ */
uint32_t g_rng = 0x1234ABCDU;          /* xorshift32; сбрасывается в Init(), поэтому результат воспроизводим */

inline float Noise()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(int32_t)g_rng * (1.0f / 2147483648.0f);   /* [-1, 1) */
}

float g_lfo_phase = 0.0f;              /* фаза общего LFO вибрато, 0..1 */

/* Резонанс корпуса: полосовой фильтр с единичным усилением на центральной частоте (RBJ), форма Transposed DF-II. */
struct Biquad {
    float b0 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void SetBandpass(float freq, float q)
    {
        const float w0    = kTwoPi * freq / kSampleRate;
        const float alpha = std::sin(w0) / (2.0f * q);
        const float inv   = 1.0f / (1.0f + alpha);
        b0 = alpha * inv;
        b2 = -alpha * inv;
        a1 = -2.0f * std::cos(w0) * inv;
        a2 = (1.0f - alpha) * inv;
    }
    void Reset() { z1 = 0.0f; z2 = 0.0f; }
    inline float Process(float x)
    {
        const float y = b0 * x + z1;
        z1 = -a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

/* Резонансы корпуса скрипки (по литературе: воздушный резонанс A0 ≈ 275 Гц, резонансы деки B1- / B1+ около
 * 450-550 Гц, «горб подставки» 2-3 кГц). Добротности занижены против реального инструмента: ради ровности
 * громкости по клавиатуре (избегаем «волчьих» нот). gain — вклад резонанса в сумму с прямым сигналом. */
struct BodyMode { float freq, q, gain; };
constexpr BodyMode kBody[] = {
    {  275.0f, 9.0f, 0.55f },   /* A0: воздушный резонанс (Гельмгольц) */
    {  455.0f, 8.0f, 0.60f },   /* T1 / B1-: основной резонанс деки */
    {  545.0f, 7.0f, 0.50f },   /* B1+ */
    {  710.0f, 6.0f, 0.40f },
    { 1150.0f, 5.0f, 0.35f },
    { 2350.0f, 2.0f, 0.45f },   /* горб подставки */
    { 3400.0f, 2.5f, 0.30f },
};
constexpr int   kBodyModes   = (int)(sizeof(kBody) / sizeof(kBody[0]));
constexpr float kBodyDry     = 0.45f;     /* прямой сигнал в сумме с резонансами */
constexpr float kBodyDetuneL = 0.990f;    /* каналы L/R: дека чуть «расстроена» - широкий стерео-образ */
constexpr float kBodyDetuneR = 1.012f;

Biquad g_body_l[kBodyModes];
Biquad g_body_r[kBodyModes];
float  g_hp_x1 = 0.0f, g_hp_y1 = 0.0f;    /* ФВЧ ~20 Гц: убирает постоянную составляющую от смычка */
float  g_lp_l  = 0.0f, g_lp_r  = 0.0f;    /* общий ФНЧ ~9 кГц на выходе */
constexpr float kHpCoef = 0.99738f;       /* exp(-2π·20/48000) */
constexpr float kOutLpK = 0.69f;          /* 1 - exp(-2π·9000/48000) */

float g_prev_gain = 0.0f;                 /* масштаб числа голосов на конце прошлого блока */
float g_expr_s    = 1.0f;                 /* сглаженная экспрессия (с учётом aftertouch) */
float g_press_s   = 0.55f;                /* сглаженный прижим */
float g_bright_s  = 0.55f;                /* сглаженная яркость */
float g_bend_s    = 0.0f;                 /* сглаженный Pitch Bend (в полутонах) */
bool  g_snap      = true;                 /* первый блок после Init(): сглаженные значения = целевым */

/* ------------------------------------------------------------------------------------------------------------
 * Вспомогательные функции
 * ------------------------------------------------------------------------------------------------------------ */
inline float Clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

/* Сворачивает ноту в рабочий диапазон целым числом октав. */
inline int FoldNote(int note)
{
    while (note < kMinNote) note += 12;
    while (note > kMaxNote) note -= 12;
    return note;
}

/* Запуск ноты в голосе «с нуля»: линии задержки очищаются, огибающая стартует с нуля. */
void StartVoice(Voice &v, uint8_t note, float vel)
{
    std::memset(v.neck, 0, sizeof(v.neck));
    std::memset(v.bridge, 0, sizeof(v.bridge));
    v.neck_w = 0;
    v.bridge_w = 0;
    v.ap_y = 0.0f;
    v.lp_y = 0.0f;
    v.dc_x1 = 0.0f;
    v.dc_y1 = 0.0f;

    const float f0 = synth::MidiFreq((uint8_t)FoldNote(note));
    v.total_len = kSampleRate / f0;
    v.pitch_gain = (f0 < 250.0f) ? std::pow(f0 * (1.0f / 250.0f), 0.35f) : 1.0f;

    /* Длина ветви «подставка» - целая, по положению смычка на момент нажатия. */
    const float beta = kBowPosMin + kBowPosRange * g_bow_pos.load(std::memory_order_relaxed);
    float lb = std::floor(beta * v.total_len + 0.5f);
    lb = Clamp(lb, 2.0f, (float)(kBridgeSize - 4U));
    v.bridge_len = (uint32_t)lb;

    v.vel = vel;
    v.bow_peak = 0.04f + 0.12f * vel;                    /* скорость смычка растёт с velocity */
    v.env = 0.0f;
    v.level = 1.0f;
    v.burst = 1.0f;
    v.age_samples = 0;
    v.loop_gain = kSustainGain;

    /* Время разгона смычка: сильный удар - быстрая атака (25 мс), тихий - медленная (90 мс); масштаб - CC73. */
    const float att_scale = std::exp2((g_attack.load(std::memory_order_relaxed) - 0.5f) * 3.0f);
    const float att_time  = (0.090f + (0.025f - 0.090f) * vel) * att_scale;
    v.att_coef = 1.0f - std::exp(-3.0f / (kSampleRate * att_time));   /* ~95% за att_time */

    v.fade_left = 0;
    v.gate = true;
    v.kill = false;
    v.sustain_held = false;
    v.order = ++g_order_counter;
}

/* Свободен ли голос и можно ли занять его мгновенно (ничего не звучит). */
inline bool VoiceIsIdle(const Voice &v)
{
    return v.note_state.load(std::memory_order_relaxed) < 0;
}

/* Выбор голоса под новую ноту: тот же номер -> свободный -> самый тихий затухающий -> самый старый. */
int AllocateVoice(uint8_t note)
{
    for (int i = 0; i < kVoices; ++i) {
        if (g_voices[i].note_state.load(std::memory_order_relaxed) == (int32_t)note) return i;
    }
    for (int i = 0; i < kVoices; ++i) {
        if (VoiceIsIdle(g_voices[i])) return i;
    }
    int   best = -1;
    float best_level = 1.0e9f;
    for (int i = 0; i < kVoices; ++i) {
        const Voice &v = g_voices[i];
        if (!v.gate && !v.sustain_held && v.fade_left == 0 && v.level < best_level) {
            best = i;
            best_level = v.level;
        }
    }
    if (best >= 0) return best;

    uint32_t oldest = 0xFFFFFFFFU;
    best = 0;
    for (int i = 0; i < kVoices; ++i) {
        if (g_voices[i].order < oldest) {
            oldest = g_voices[i].order;
            best = i;
        }
    }
    return best;
}

void HandleNoteOn(const ViolinEvent &ev)
{
    const int idx = AllocateVoice(ev.note);
    Voice &v = g_voices[idx];
    const int32_t cur = v.note_state.load(std::memory_order_relaxed);

    if (cur == (int32_t)ev.note) {
        /* Повторное нажатие той же ноты: струна продолжает звучать, смычок «берёт» заново (без обрыва). */
        if (v.press_count < 255) v.press_count++;
        v.gate = true;
        v.kill = false;
        v.sustain_held = false;
        v.vel = ev.vel;
        v.bow_peak = 0.04f + 0.12f * ev.vel;
        v.burst = 1.0f;
        v.order = ++g_order_counter;
        if (v.fade_left > 0) v.pending.vel = ev.vel;
        return;
    }

    if (cur >= 0) {
        /* Голос занят другой нотой: сначала гасим её (2,7 мс), новая стартует после гашения - без щелчка. */
        g_input.ReleasePress((uint8_t)cur);
        v.pending.note = ev.note;
        v.pending.vel  = ev.vel;
        v.fade_left    = kFadeSamples;
        v.press_count  = 1;
        v.gate         = true;
        v.kill         = false;
        v.sustain_held = false;
        v.order        = ++g_order_counter;
        v.note_state.store((int32_t)ev.note, std::memory_order_release);
        return;
    }

    StartVoice(v, ev.note, ev.vel);
    v.press_count = 1;
    v.note_state.store((int32_t)ev.note, std::memory_order_release);
}

void HandleNoteOff(uint8_t note)
{
    for (int i = 0; i < kVoices; ++i) {
        Voice &v = g_voices[i];
        if (v.note_state.load(std::memory_order_relaxed) == (int32_t)note && v.press_count > 0) {
            v.press_count--;
            if (v.press_count == 0) {
                if (g_pedal) {
                    v.sustain_held = true;
                } else {
                    v.sustain_held = false;
                    v.gate = false;
                }
            }
        }
    }
}

/* All Sound Off: смычок снимается со всех струн, педаль и удержания сбрасываются, спад ~20 мс (без щелчка). */
void HandleKill()
{
    g_pedal = false;
    for (int i = 0; i < kVoices; ++i) {
        Voice &v = g_voices[i];
        v.press_count = 0;
        v.sustain_held = false;
        v.gate = false;
        v.kill = true;
        v.fade_left = 0;
    }
}

void HandleSustain(uint8_t value)
{
    const bool on = (value >= 64);
    g_pedal = on;
    if (!on) {
        for (int i = 0; i < kVoices; ++i) {
            Voice &v = g_voices[i];
            v.sustain_held = false;
            if (v.press_count == 0) v.gate = false;
        }
    }
}

/* Параметры блока, общие для всех голосов (считаются один раз на подблок). */
struct ChunkCtx {
    float        pb_scale;      /* множитель длины петли от Pitch Bend (< 1 - выше по тону) */
    float        expr;          /* экспрессия с учётом aftertouch */
    float        pressure;      /* базовый прижим 0..1 */
    float        lp_pole;       /* полюс фильтра потерь подставки: больше - темнее */
    float        noise_base;    /* постоянный шум смычка */
    float        vib_frac;      /* глубина вибрато (доля длины петли) */
    float        rel_coef;      /* коэффициент снятия смычка за отсчёт */
    float        ring_release;  /* постоянная затухания струны после отпускания, с */
    float        kill_rel_coef; /* то же для All Sound Off (быстрый спад) */
    float        kill_ring;
    const float *lfo;           /* значения LFO на подблок (-1..1) */
};

/* Рендер одного голоса на n (<= kBlock) отсчётов: результат ДОБАВЛЯЕТСЯ в mix. Возвращает true, если голос
 * надо освободить (смычок снят, сигнал затух). */
bool RenderVoice(Voice &v, uint32_t n, const ChunkCtx &c, float *mix)
{
    const bool  gate_on = v.gate || v.sustain_held;
    const float D       = v.total_len * c.pb_scale;
    const float f0      = kSampleRate / D;
    const float w0      = kTwoPi / D;
    const float p       = c.lp_pole;

    /* Фазовая задержка фильтра потерь на основной частоте: её нужно вычесть из длины ветви «гриф» для точного строя. */
    const float d_lp = std::atan2(p * std::sin(w0), 1.0f - p * std::cos(w0)) / w0;
    float neck_base  = D - (float)v.bridge_len - d_lp;

    /* Эмпирическая поправка строя: нелинейная петля синхронизируется по групповой задержке фильтров, а не по фазовой
     * на основной частоте, из-за чего ноты выше ~500 Гц звучали на 5-15 центов выше (измерено автокорреляцией и
     * БПФ по всему диапазону). Поправка линейна по частоте и ограничена сверху. */
    neck_base += D * kPitchTrim * std::min(f0, 1100.0f);


    /* All-pass интерполятор даёт задержку delta только на низких частотах; на частоте основного тона его фазовая
     * задержка немного меньше (ноты выше ~1 кГц иначе звучали бы заметно выше строя). Считаем фазовую задержку
     * all-pass на w0 точно и добавляем разницу к длине ветви «гриф». */
    {
        const float nf0 = std::floor(neck_base - 0.5f);
        const float d0  = neck_base - nf0;
        const float a0  = (1.0f - d0) / (1.0f + d0);
        const float sw  = std::sin(w0);
        const float cw  = std::cos(w0);
        const float arg = std::atan2(-sw, a0 + cw) - std::atan2(-a0 * sw, 1.0f + a0 * cw);
        neck_base -= (-arg / w0) - d0;
    }

    /* Потери за период: после отпускания смычка струна быстро затухает (палец/смычок гасят её), пока смычок на
     * струне - потери малы. Постоянная времени от высоты не зависит. */
    const float g_tgt = gate_on ? kSustainGain : std::exp(-1.0f / (f0 * (v.kill ? c.kill_ring : c.ring_release)));
    v.loop_gain += (g_tgt - v.loop_gain) * 0.25f;
    const float g = v.loop_gain;

    /* Прижим смычка -> наклон таблицы трения; чем сильнее удар (velocity), тем сильнее прижим. */
    const float press = Clamp(c.pressure + kPressVelK * (v.vel - 0.5f), 0.0f, 1.0f);
    const float slope = 5.0f - 4.0f * press;

    /* Шум смычка: постоянный + всплеск при атаке. */
    const float noise_burst_gain = 0.35f * (0.4f + 0.6f * v.vel);

    /* Вибрато: включается с задержкой и плавно нарастает (smoothstep). */
    auto vib_amount = [&](uint32_t age) {
        const float t = (((float)age / kSampleRate) - kVibratoDelay) / kVibratoRise;
        const float u = Clamp(t, 0.0f, 1.0f);
        return u * u * (3.0f - 2.0f * u);
    };
    const float vib0 = vib_amount(v.age_samples) * c.vib_frac;
    const float vib1 = vib_amount(v.age_samples + n) * c.vib_frac;

    const float env_target = gate_on ? 1.0f : 0.0f;
    const float env_coef   = gate_on ? v.att_coef : (v.kill ? c.kill_rel_coef : c.rel_coef);

    /* Плавное гашение вытесняемого голоса. */
    const bool  fading = v.fade_left > 0;
    const float fg0 = fading ? (float)v.fade_left / (float)kFadeSamples : 1.0f;
    const float fg1 = fading ? (float)std::max<int32_t>(v.fade_left - (int32_t)n, 0) / (float)kFadeSamples : 1.0f;

    /* Локальные копии состояния - компилятор держит их в регистрах. */
    float    env = v.env, burst = v.burst, lp = v.lp_y, ap_y = v.ap_y, dc_x1 = v.dc_x1, dc_y1 = v.dc_y1;
    uint32_t nw = v.neck_w, bw = v.bridge_w;
    const uint32_t lb = v.bridge_len;
    const float bow_scale = v.bow_peak * c.expr;
    const float burst_decay = 0.99896f;          /* exp(-1/(0.02 с · 48000)) */
    const float inv_n = 1.0f / (float)n;
    float chunk_max = 0.0f;

    for (uint32_t k = 0; k < n; ++k) {
        env   += (env_target - env) * env_coef;
        burst *= burst_decay;

        /* Длина ветви «гриф» с вибрато; N - целая часть, delta в [0.5, 1.5) - дробная (область устойчивости all-pass). */
        const float vib = c.lfo[k] * (vib0 + (vib1 - vib0) * ((float)k * inv_n));
        const float ln  = Clamp(neck_base + D * vib, 2.0f, (float)(kNeckSize - 3U));
        const float nf  = std::floor(ln - 0.5f);
        const uint32_t N = (uint32_t)nf;
        const float delta = ln - nf;
        const float a   = (1.0f - delta) / (1.0f + delta);

        /* Отражение от подставки (через фильтр потерь) и от порожка (инверсия). */
        const float b_out = v.bridge[(bw - lb) & kBridgeMask];
        lp = (1.0f - p) * b_out + p * lp;
        const float dc_out = lp - dc_x1 + kDcBlockR * dc_y1;   /* ФВЧ: y = x - x[n-1] + R·y[n-1] */
        dc_x1 = lp;
        dc_y1 = dc_out;
        const float bridge_refl = -dc_out * g;

        const float xd  = v.neck[(nw - N) & kNeckMask];
        const float xd1 = v.neck[(nw - N - 1U) & kNeckMask];
        const float y   = a * (xd - ap_y) + xd1;     /* all-pass: y = a·xd + xd1 − a·y[n−1] */
        ap_y = y;
        const float nut_refl = -y;

        /* Смычок: скорость струны под смычком, разность скоростей и нелинейная таблица трения. */
        const float string_v = bridge_refl + nut_refl;
        const float noise_g  = c.noise_base + noise_burst_gain * burst;
        const float bow_v    = bow_scale * env * (1.0f + noise_g * Noise());
        const float dv       = bow_v - string_v;
        float t = std::fabs(dv * slope) + 0.75f;
        t *= t;
        t = 1.0f / (t * t);                          /* |x + 0.75|^-4 (таблица трения STK) */
        t = Clamp(t, 0.01f, 0.99f);
        const float new_v = dv * t;

        v.neck[nw & kNeckMask]       = bridge_refl + new_v;
        v.bridge[bw & kBridgeMask]   = nut_refl + new_v;
        ++nw;
        ++bw;

        const float fg = fg0 + (fg1 - fg0) * ((float)k * inv_n);
        mix[k] += b_out * fg * v.pitch_gain;
        const float ab = std::fabs(b_out);
        if (ab > chunk_max) chunk_max = ab;
    }

    v.env = env;  v.burst = burst;  v.lp_y = lp;  v.ap_y = ap_y;  v.dc_x1 = dc_x1;  v.dc_y1 = dc_y1;
    v.neck_w = nw;  v.bridge_w = bw;
    v.age_samples += n;
    v.level = std::max(chunk_max, v.level * 0.9f);

    /* Закончилось гашение: стартует отложенная нота. */
    if (fading) {
        v.fade_left -= (int32_t)n;
        if (v.fade_left <= 0) {
            const PendingNote pn = v.pending;
            const uint8_t cnt = v.press_count;
            StartVoice(v, pn.note, pn.vel);
            v.press_count = cnt;
            return false;
        }
        return false;
    }

    return !v.gate && !v.sustain_held && v.env < 1.0e-3f && v.level < 1.0e-4f;
}

}  // namespace

/* ------------------------------------------------------------------------------------------------------------
 * Публичный API
 * ------------------------------------------------------------------------------------------------------------ */
extern "C" {

void ViolinSynth_Init(void)
{
    synth::MidiFreqInit();

    /* Голоса создаются в общей арене заново: данные прежнего владельца (другого движка) стираются. */
    g_voices = SynthArena_NewArray<Voice, VIOLIN_VOICES>(&g_voices);

    g_order_counter = 0;
    g_pedal = false;
    g_rng = 0x1234ABCDU;
    g_lfo_phase = 0.0f;

    if (g_voices) {
        for (int i = 0; i < kVoices; ++i) {
            g_voices[i].note_state.store(-1, std::memory_order_relaxed);
        }
    }

    for (int m = 0; m < kBodyModes; ++m) {
        g_body_l[m].SetBandpass(kBody[m].freq * kBodyDetuneL, kBody[m].q);
        g_body_r[m].SetBandpass(kBody[m].freq * kBodyDetuneR, kBody[m].q);
        g_body_l[m].Reset();
        g_body_r[m].Reset();
    }
    g_hp_x1 = g_hp_y1 = 0.0f;
    g_lp_l = g_lp_r = 0.0f;
    g_prev_gain = 0.0f;
    g_snap = true;

    g_vibrato.store(0.0f, std::memory_order_relaxed);
    g_expression.store(1.0f, std::memory_order_relaxed);
    g_bow_pos.store(0.38f, std::memory_order_relaxed);
    g_pressure.store(0.55f, std::memory_order_relaxed);
    g_release.store(0.5f, std::memory_order_relaxed);
    g_attack.store(0.5f, std::memory_order_relaxed);
    g_bright.store(0.55f, std::memory_order_relaxed);
    g_noise.store(0.4f, std::memory_order_relaxed);
    g_bend.store(0, std::memory_order_relaxed);
    g_aftertouch.store(0.0f, std::memory_order_relaxed);

    g_input.Reset();
}

void ViolinSynth_NoteOn(uint8_t midi_note, uint8_t velocity)
{
    if (velocity == 0) {
        ViolinSynth_NoteOff(midi_note);
        return;
    }
    if (midi_note >= 128) return;

    ViolinEvent ev;
    ev.type  = EV_NOTE_ON;
    ev.note  = midi_note;
    ev.value = velocity;
    ev.vel   = Clamp((float)velocity * (1.0f / 127.0f), 0.0f, 1.0f);
    g_input.NoteOn(midi_note, ev);
}

void ViolinSynth_NoteOff(uint8_t midi_note)
{
    if (midi_note >= 128) return;

    ViolinEvent ev;
    ev.type  = EV_NOTE_OFF;
    ev.note  = midi_note;
    ev.value = 0;
    ev.vel   = 0.0f;
    g_input.NoteOff(midi_note, ev);
}

void ViolinSynth_ControlChange(uint8_t control, uint8_t value)
{
    const float norm = (float)(value > 127 ? 127 : value) * (1.0f / 127.0f);

    switch (control) {
        case 1:  g_vibrato.store(norm, std::memory_order_relaxed);     break;   /* мод-колесо: прибавка к вибрато по умолчанию */
        case 2:
        case 11: g_expression.store(norm, std::memory_order_relaxed);  break;
        case 70: g_bow_pos.store(norm, std::memory_order_relaxed);     break;
        case 71: g_pressure.store(norm, std::memory_order_relaxed);    break;
        case 72: g_release.store(norm, std::memory_order_relaxed);     break;
        case 73: g_attack.store(norm, std::memory_order_relaxed);      break;
        case 74: g_bright.store(norm, std::memory_order_relaxed);      break;
        case 75: g_noise.store(norm, std::memory_order_relaxed);       break;
        case 120: {   /* All Sound Off: критичное событие (потеря = залипшие ноты) */
            ViolinEvent ev;
            ev.type  = EV_KILL;
            ev.note  = 0;
            ev.value = 0;
            ev.vel   = 0.0f;
            g_input.Critical(ev);
            break;
        }
        case 64: {
            ViolinEvent ev;
            ev.type  = EV_SUSTAIN;
            ev.note  = 0;
            ev.value = value;
            ev.vel   = 0.0f;
            g_input.Critical(ev);
            break;
        }
        default: break;
    }
}

void ViolinSynth_PitchBend(int16_t bend)
{
    g_bend.store(bend < -8192 ? -8192 : (bend > 8191 ? 8191 : (int32_t)bend), std::memory_order_relaxed);
}

void ViolinSynth_ChannelPressure(uint8_t pressure)
{
    g_aftertouch.store((float)(pressure > 127 ? 127 : pressure) * (1.0f / 127.0f), std::memory_order_relaxed);
}

bool ViolinSynth_IsNoteActive(uint8_t midi_note)
{
    if (!SynthArena_IsOwner(&g_voices) || g_voices == nullptr) return false;
    for (int i = 0; i < kVoices; ++i) {
        if (g_voices[i].note_state.load(std::memory_order_relaxed) == (int32_t)midi_note) return true;
    }
    return false;
}

uint32_t ViolinSynth_GetDroppedEventsCount(void)
{
    return g_input.Dropped();
}

void ViolinSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames)
{
    if (!SynthArena_IsOwner(&g_voices) || g_voices == nullptr) {   /* арену забрал другой движок или Init() не вызывался */
        std::memset(buffer, 0, (size_t)num_frames * 2U * sizeof(int16_t));
        return;
    }

    /* 1. Аварийное восстановление очереди (см. synth_common.h): гасим все голоса и педаль. */
    g_input.ServiceRecovery([]() {
        g_pedal = false;
        for (int i = 0; i < kVoices; ++i) {
            Voice &v = g_voices[i];
            v.press_count = 0;
            v.sustain_held = false;
            v.gate = false;
            v.fade_left = 0;
            v.env = 0.0f;
            v.level = 0.0f;
            v.note_state.store(-1, std::memory_order_relaxed);
        }
    });

    /* 2. События из очереди. */
    ViolinEvent ev;
    while (g_input.Pop(ev)) {
        switch (ev.type) {
            case EV_NOTE_ON:  HandleNoteOn(ev);        break;
            case EV_NOTE_OFF: HandleNoteOff(ev.note);  break;
            case EV_SUSTAIN:  HandleSustain(ev.value); break;
            case EV_KILL:     HandleKill();            break;
        }
    }

    /* 3. Параметры на весь вызов. */
    const float tgt_expr   = g_expression.load(std::memory_order_relaxed)
                           * (1.0f + 0.5f * g_aftertouch.load(std::memory_order_relaxed));
    const float tgt_press  = g_pressure.load(std::memory_order_relaxed);
    const float tgt_bright = g_bright.load(std::memory_order_relaxed);
    const float tgt_bend   = (float)g_bend.load(std::memory_order_relaxed) * (kBendSemitones / 8192.0f);
    const float vib_mod    = g_vibrato.load(std::memory_order_relaxed);
    const float vib_depth  = kVibratoBase + (1.0f - kVibratoBase) * vib_mod;   /* колесо в нуле: kVibratoBase */
    const float noise      = g_noise.load(std::memory_order_relaxed);
    const float rel_scale  = std::exp2((g_release.load(std::memory_order_relaxed) - 0.5f) * 3.0f);

    if (g_snap) {
        g_expr_s = tgt_expr;  g_press_s = tgt_press;  g_bright_s = tgt_bright;  g_bend_s = tgt_bend;
        g_snap = false;
    }

    const float rel_time = kReleaseBase * rel_scale;

    uint32_t remaining = num_frames;
    int16_t *out = buffer;

    while (remaining > 0) {
        const uint32_t n = std::min(kBlock, remaining);

        /* Сглаживание параметров (постоянная времени ≈ 7 мс) - без «ступенек» при вращении ручек. */
        g_expr_s   += (tgt_expr   - g_expr_s)   * 0.10f;
        g_press_s  += (tgt_press  - g_press_s)  * 0.10f;
        g_bright_s += (tgt_bright - g_bright_s) * 0.10f;
        g_bend_s   += (tgt_bend   - g_bend_s)   * 0.20f;

        /* LFO вибрато на подблок. */
        float lfo[kBlock];
        const float lfo_step = kVibratoHz / kSampleRate;
        for (uint32_t k = 0; k < n; ++k) {
            lfo[k] = std::sin(kTwoPi * g_lfo_phase);
            g_lfo_phase += lfo_step;
            if (g_lfo_phase >= 1.0f) g_lfo_phase -= 1.0f;
        }

        ChunkCtx c;
        c.pb_scale     = std::exp2(-g_bend_s * (1.0f / 12.0f));   /* +1 полутон -> петля короче -> тон выше */
        c.expr         = g_expr_s;
        c.pressure     = g_press_s;
        c.lp_pole      = 0.80f - 0.55f * g_bright_s;
        c.noise_base   = 0.015f + 0.06f * noise;
        c.vib_frac     = vib_depth * kVibratoMaxFrac;
        c.rel_coef     = 1.0f - std::exp(-3.0f / (kSampleRate * rel_time));
        c.ring_release = kRingReleaseK * rel_time;
        c.kill_rel_coef = 1.0f - std::exp(-3.0f / (kSampleRate * kKillTime));
        c.kill_ring     = kRingReleaseK * kKillTime;
        c.lfo          = lfo;

        float mix[kBlock];
        std::memset(mix, 0, sizeof(mix));
        int active = 0;

        for (int i = 0; i < kVoices; ++i) {
            Voice &v = g_voices[i];
            if (v.note_state.load(std::memory_order_relaxed) < 0) continue;
            ++active;
            if (RenderVoice(v, n, c, mix)) {
                v.press_count = 0;
                v.gate = false;
                v.sustain_held = false;
                v.note_state.store(-1, std::memory_order_release);
            }
        }

        /* Громкость падает с числом звучащих голосов (аккорды не клиппируют); меняется плавно внутри подблока. */
        const float gain_tgt = kOutGain / std::sqrt((float)std::max(active, 1));
        const float gain_step = (gain_tgt - g_prev_gain) / (float)n;
        float gain = g_prev_gain;
        g_prev_gain = gain_tgt;

        for (uint32_t k = 0; k < n; ++k) {
            gain += gain_step;
            /* ФВЧ: убираем постоянную составляющую. */
            const float x = mix[k] * gain;
            const float hp = x - g_hp_x1 + kHpCoef * g_hp_y1;
            g_hp_x1 = x;
            g_hp_y1 = hp;

            /* Дека: прямой сигнал + параллельный банк резонансов (отдельный набор для каждого канала). */
            float yl = kBodyDry * hp;
            float yr = kBodyDry * hp;
            for (int m = 0; m < kBodyModes; ++m) {
                yl += kBody[m].gain * g_body_l[m].Process(hp);
                yr += kBody[m].gain * g_body_r[m].Process(hp);
            }
            g_lp_l += kOutLpK * (yl - g_lp_l);
            g_lp_r += kOutLpK * (yr - g_lp_r);

            const float sl = synth::SoftLimit(g_lp_l, 0.80f, 0.98f);
            const float sr = synth::SoftLimit(g_lp_r, 0.80f, 0.98f);
            *out++ = (int16_t)(sl * 32767.0f);
            *out++ = (int16_t)(sr * 32767.0f);
        }

        remaining -= n;
    }
}

} /* extern "C" */

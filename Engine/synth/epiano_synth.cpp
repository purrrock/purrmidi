#include "epiano_synth.h"
#include <atomic>
#include <cmath>
#include <cstring>

/* ------------------------------------------------------------------------- */
/*  Настройки (можно переопределить через -D при компиляции)                 */
/* ------------------------------------------------------------------------- */

#ifndef EPIANO_NUM_VOICES
#define EPIANO_NUM_VOICES       8       /* полифония */
#endif

#ifndef EPIANO_STEREO_SPREAD
#define EPIANO_STEREO_SPREAD    0.30f   /* ширина панорамы по клавиатуре, 0 = моно */
#endif

#define EPIANO_SAMPLE_RATE      48000.0f
#define EPIANO_TABLE_SIZE       2048                    /* степень двойки */
#define EPIANO_TABLE_MASK       (EPIANO_TABLE_SIZE - 1)
#define EPIANO_EVENT_FIFO_SIZE  64                      /* степень двойки */
#define EPIANO_BLOCK            64                      /* размер внутреннего блока рендера */

#define EPIANO_MASTER_GAIN      0.40f   /* общий уровень; запас под аккорды */
#define EPIANO_TINE_RATIO       14.0f   /* частота модулятора "язычка" относительно ноты */

#define EPIANO_ATTACK_SEC       0.002f  /* антищелчковая атака */
#define EPIANO_BARK_T60_SEC     1.10f   /* затухание яркости корпуса */
#define EPIANO_TINE_T60_SEC     0.30f   /* затухание "звона" */
#define EPIANO_DEFAULT_REL_SEC  0.25f   /* T60 релиза (демпфер) по умолчанию */
#define EPIANO_SILENCE          1.0e-4f /* порог отключения голоса (-80 дБ) */

#define EPIANO_TWO_PI           6.28318530717958647692f
#define EPIANO_LN_1000          6.90775527898213705205f /* ln(10^3) = -60 дБ */

/* ------------------------------------------------------------------------- */
/*  Состояние                                                                */
/* ------------------------------------------------------------------------- */

namespace {

enum EventType : uint8_t { EV_NOTE_ON = 0, EV_NOTE_OFF = 1, EV_CC = 2 };

struct Event {
    uint8_t type;
    uint8_t a;      /* note / controller */
    uint8_t b;      /* velocity / value  */
};

struct Voice {
    bool     active;
    bool     gate;          /* клавиша удерживается */
    bool     kill;          /* All Sound Off: быстрое затухание независимо от клавиши/педали */
    uint8_t  note;
    uint32_t age;           /* порядковый номер запуска (для voice stealing) */

    float phase;            /* общая фаза несущей и модулятора BODY (оба 1:1) */
    float phase_tine_mod;   /* фаза модулятора TINE (14:1) */
    float inc;              /* приращение фазы за сэмпл (f / fs) */

    float level;            /* общая огибающая амплитуды */
    bool  attacking;
    float attack_inc;
    float decay_coeff;      /* множитель за сэмпл, пока клавиша/педаль держат ноту */

    float bark;             /* огибающая индекса BODY, 1 -> 0 */
    float bark_coeff;
    float tine_env;         /* огибающая индекса TINE, 1 -> 0 */
    float tine_coeff;

    float body_idx_floor;   /* индекс BODY в "циклах" (рад / 2pi), постоянная часть */
    float body_idx_peak;    /* добавка, затухающая вместе с bark */
    float tine_idx;         /* индекс TINE в "циклах" */
    float tine_level;       /* уровень TINE в миксе */

    float vel_gain;
    float gain_l;
    float gain_r;
};

float          g_sine_table[EPIANO_TABLE_SIZE];
bool           g_table_ready = false;

Voice          g_voices[EPIANO_NUM_VOICES];
uint32_t       g_age_counter = 0;

/* Состояние контроллеров — принадлежит аудиоконтексту */
bool           g_sustain      = false;
float          g_brightness   = 1.0f;
float          g_volume       = 1.0f;
float          g_release_coeff = 0.0f;
float          g_kill_coeff   = 0.0f;
float          g_attack_inc   = 0.0f;
float          g_bark_coeff   = 0.0f;
float          g_tine_coeff   = 0.0f;

/* Lock-free SPSC FIFO: производитель — main, потребитель — аудио */
Event                  g_fifo[EPIANO_EVENT_FIFO_SIZE];
std::atomic<uint32_t>  g_fifo_head{0};
std::atomic<uint32_t>  g_fifo_tail{0};
std::atomic<uint32_t>  g_dropped{0};

/* ------------------------------------------------------------------------- */
/*  Вспомогательные функции                                                  */
/* ------------------------------------------------------------------------- */

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Коэффициент экспоненциального спада на -60 дБ за t60 секунд */
inline float t60_to_coeff(float t60_sec)
{
    return expf(-EPIANO_LN_1000 / (EPIANO_SAMPLE_RATE * t60_sec));
}

inline float midi_to_hz(uint8_t note)
{
    return 440.0f * exp2f(((float)note - 69.0f) * (1.0f / 12.0f));
}

/* sin(2*pi*x), x — фаза в циклах, x >= 0. Табличный синус с линейной интерполяцией. */
inline float sine_cycles(float x)
{
    float pos = x * (float)EPIANO_TABLE_SIZE;
    int32_t i = (int32_t)pos;
    float frac = pos - (float)i;
    float s0 = g_sine_table[i & EPIANO_TABLE_MASK];
    float s1 = g_sine_table[(i + 1) & EPIANO_TABLE_MASK];
    return s0 + frac * (s1 - s0);
}

/* Мягкий лимитер: до 0.6 — линейно, выше — плавно к 1.0 (без жёсткого клиппинга) */
inline float soft_limit(float x)
{
    float ax = x < 0.0f ? -x : x;
    if (ax <= 0.6f) {
        return x;
    }
    float t = (ax - 0.6f) * 2.5f;           /* (ax - 0.6) / 0.4 */
    float y = 0.6f + 0.4f * (t / (1.0f + t));
    return x < 0.0f ? -y : y;
}

bool fifo_push(const Event& ev)
{
    uint32_t head = g_fifo_head.load(std::memory_order_relaxed);
    uint32_t tail = g_fifo_tail.load(std::memory_order_acquire);

    if (head - tail >= EPIANO_EVENT_FIFO_SIZE) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_fifo[head & (EPIANO_EVENT_FIFO_SIZE - 1)] = ev;
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
    ev = g_fifo[tail & (EPIANO_EVENT_FIFO_SIZE - 1)];
    g_fifo_tail.store(tail + 1, std::memory_order_release);
    return true;
}

/* ------------------------------------------------------------------------- */
/*  Голоса                                                                   */
/* ------------------------------------------------------------------------- */

/* Выбор голоса: тот же звук -> свободный -> самый тихий отпущенный -> самый старый */
Voice* allocate_voice(uint8_t note)
{
    for (int i = 0; i < EPIANO_NUM_VOICES; i++) {
        if (g_voices[i].active && g_voices[i].note == note) {
            return &g_voices[i];
        }
    }
    for (int i = 0; i < EPIANO_NUM_VOICES; i++) {
        if (!g_voices[i].active) {
            return &g_voices[i];
        }
    }

    Voice* best = nullptr;
    for (int i = 0; i < EPIANO_NUM_VOICES; i++) {
        Voice* v = &g_voices[i];
        if (!v->gate && (!best || v->level < best->level)) {
            best = v;
        }
    }
    if (best) {
        return best;
    }

    best = &g_voices[0];
    for (int i = 1; i < EPIANO_NUM_VOICES; i++) {
        if (g_voices[i].age < best->age) {
            best = &g_voices[i];
        }
    }
    return best;
}

void start_voice(uint8_t note, uint8_t velocity)
{
    Voice* v = allocate_voice(note);

    const bool fresh = !v->active || v->note != note;
    const float vel  = clampf((float)velocity * (1.0f / 127.0f), 0.0f, 1.0f);
    const float freq = clampf(midi_to_hz(note), 8.0f, EPIANO_SAMPLE_RATE * 0.45f);
    const float nf   = (float)note;

    if (fresh) {
        /* новая нота: фазы с нуля; если голос украден, уровень не обнуляем —
         * атака поднимается от текущего значения, чтобы избежать щелчка */
        v->phase          = 0.0f;
        v->phase_tine_mod = 0.0f;
        if (!v->active) {
            v->level = 0.0f;
        }
    }

    v->active    = true;
    v->gate      = true;
    v->kill      = false;
    v->note      = note;
    v->age       = ++g_age_counter;
    v->inc       = freq / EPIANO_SAMPLE_RATE;

    /* Затухание удерживаемой ноты (T60): 18 с на C2, вдвое быстрее на каждые 2,5 октавы выше
     * (C4 ~ 10 с, C6 ~ 4,5 с) */
    const float hold_t60 = clampf(18.0f * exp2f(-(nf - 36.0f) * (1.0f / 30.0f)), 1.0f, 20.0f);
    v->decay_coeff = t60_to_coeff(hold_t60);

    v->attacking  = true;
    v->attack_inc = g_attack_inc;

    v->bark       = 1.0f;
    v->bark_coeff = g_bark_coeff;
    v->tine_env   = 1.0f;
    v->tine_coeff = g_tine_coeff;

    /* Индексы модуляции: растут с velocity (квадратично), падают к верхним нотам.
     * Значения в радианах, переводятся в "циклы" делением на 2*pi. */
    const float key_scale = clampf(1.15f - (nf - 48.0f) * 0.0125f, 0.20f, 1.20f);
    const float bright    = g_brightness * key_scale;
    const float vv        = vel * vel;

    v->body_idx_floor = (0.25f + 0.35f * vv) * bright * (1.0f / EPIANO_TWO_PI);
    v->body_idx_peak  = (0.35f + 2.40f * vv) * bright * (1.0f / EPIANO_TWO_PI);

    /* "Звон" не должен создавать алиасинг: модулятор на частоте 14*f.
     * Плавно гасим его, когда 14*f приближается к 18 кГц. */
    const float tine_alias_fade = clampf((18000.0f - EPIANO_TINE_RATIO * freq) * (1.0f / 8000.0f), 0.0f, 1.0f);
    v->tine_idx   = (0.10f + 1.30f * vv) * g_brightness * tine_alias_fade * (1.0f / EPIANO_TWO_PI);
    v->tine_level = (0.18f + 0.22f * vel) * tine_alias_fade;

    /* Динамика: velocity^1.5 с небольшим полом, чтобы pp не пропадали совсем */
    v->vel_gain = 0.03f + 0.97f * vel * sqrtf(vel);

    /* Панорама по клавиатуре (константная сумма L+R = 1) */
    const float pan = clampf((nf - 60.0f) * (1.0f / 48.0f), -1.0f, 1.0f) * EPIANO_STEREO_SPREAD;
    v->gain_l = 0.5f * (1.0f - pan);
    v->gain_r = 0.5f * (1.0f + pan);
}

void release_note(uint8_t note)
{
    for (int i = 0; i < EPIANO_NUM_VOICES; i++) {
        if (g_voices[i].active && g_voices[i].gate && g_voices[i].note == note) {
            g_voices[i].gate = false;
        }
    }
}

void all_notes_off(bool immediate)
{
    for (int i = 0; i < EPIANO_NUM_VOICES; i++) {
        g_voices[i].gate = false;
        if (immediate) {
            g_voices[i].kill = true;    /* быстрый (но не щелчковый) спад */
        }
    }
    g_sustain = false;
}

void apply_cc(uint8_t control, uint8_t value)
{
    const float norm = (float)value * (1.0f / 127.0f);

    switch (control) {
        case 1:
        case 71:
        case 74:
            g_brightness = 0.3f + 1.4f * norm;      /* 64 -> ~1.0 */
            break;
        case 7:
            g_volume = norm;
            break;
        case 64:
            g_sustain = (value >= 64);
            break;
        case 72:
            g_release_coeff = t60_to_coeff(0.08f + 1.6f * norm * norm);
            break;
        case 120:
            all_notes_off(true);
            break;
        case 123:
            all_notes_off(false);
            break;
        default:
            break;
    }
}

void process_events()
{
    Event ev;
    while (fifo_pop(ev)) {
        switch (ev.type) {
            case EV_NOTE_ON:  start_voice(ev.a, ev.b); break;
            case EV_NOTE_OFF: release_note(ev.a);      break;
            case EV_CC:       apply_cc(ev.a, ev.b);    break;
            default: break;
        }
    }
}

/* Рендер одного голоса в аккумуляторы; при полном затухании голос освобождается */
void render_voice(Voice& v, float* acc_l, float* acc_r, uint32_t n)
{
    const bool holding_base = v.gate || g_sustain;
    const float amp_coeff   = v.kill ? g_kill_coeff
                            : (holding_base ? v.decay_coeff : g_release_coeff);

    float phase    = v.phase;
    float phase_tm = v.phase_tine_mod;
    const float inc      = v.inc;
    const float inc_tm   = v.inc * EPIANO_TINE_RATIO;
    float level  = v.level;
    float bark   = v.bark;
    float tine_e = v.tine_env;

    for (uint32_t i = 0; i < n; i++) {
        /* --- огибающая амплитуды --- */
        if (v.attacking) {
            level += v.attack_inc;
            if (level >= 1.0f) {
                level = 1.0f;
                v.attacking = false;
            }
        } else {
            level *= amp_coeff;
        }

        /* --- BODY: несущая 1:1, модулятор 1:1 (общая фаза) --- */
        const float body_idx = v.body_idx_floor + v.body_idx_peak * bark;
        const float body_mod = sine_cycles(phase);
        const float body     = sine_cycles(phase + body_idx * body_mod + 4.0f);

        /* --- TINE: несущая 1:1, модулятор 14:1 --- */
        const float tine_mod = sine_cycles(phase_tm);
        const float tine     = sine_cycles(phase + v.tine_idx * tine_e * tine_mod + 4.0f);

        const float s = (body + v.tine_level * tine) * level * v.vel_gain;

        acc_l[i] += s * v.gain_l;
        acc_r[i] += s * v.gain_r;

        /* --- продвижение фаз и огибающих индекса --- */
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        phase_tm += inc_tm;
        phase_tm -= (float)(int32_t)phase_tm;

        bark   *= v.bark_coeff;
        tine_e *= v.tine_coeff;
    }

    v.phase          = phase;
    v.phase_tine_mod = phase_tm;
    v.level          = level;
    v.bark           = bark;
    v.tine_env       = tine_e;

    if (!v.attacking && level < EPIANO_SILENCE) {
        v.active = false;
        v.gate   = false;
        v.level  = 0.0f;
    }
}

} // namespace

/* ------------------------------------------------------------------------- */
/*  Публичный API                                                            */
/* ------------------------------------------------------------------------- */

extern "C" {

void EPianoSynth_Init(void)
{
    if (!g_table_ready) {
        for (int i = 0; i < EPIANO_TABLE_SIZE; i++) {
            g_sine_table[i] = sinf(EPIANO_TWO_PI * (float)i / (float)EPIANO_TABLE_SIZE);
        }
        g_table_ready = true;
    }

    std::memset(g_voices, 0, sizeof(g_voices));
    g_age_counter = 0;

    g_sustain       = false;
    g_brightness    = 1.0f;
    g_volume        = 1.0f;
    g_release_coeff = t60_to_coeff(EPIANO_DEFAULT_REL_SEC);
    g_kill_coeff    = t60_to_coeff(0.02f);
    g_attack_inc    = 1.0f / (EPIANO_SAMPLE_RATE * EPIANO_ATTACK_SEC);
    g_bark_coeff    = t60_to_coeff(EPIANO_BARK_T60_SEC);
    g_tine_coeff    = t60_to_coeff(EPIANO_TINE_T60_SEC);

    g_fifo_head.store(0, std::memory_order_relaxed);
    g_fifo_tail.store(0, std::memory_order_relaxed);
    g_dropped.store(0, std::memory_order_relaxed);
}

void EPianoSynth_NoteOn(uint8_t midi_note, uint8_t velocity)
{
    if (velocity == 0) {
        EPianoSynth_NoteOff(midi_note);
        return;
    }
    if (midi_note > 127) {
        return;
    }
    Event ev = { EV_NOTE_ON, midi_note, (uint8_t)(velocity > 127 ? 127 : velocity) };
    fifo_push(ev);
}

void EPianoSynth_NoteOff(uint8_t midi_note)
{
    if (midi_note > 127) {
        return;
    }
    Event ev = { EV_NOTE_OFF, midi_note, 0 };
    fifo_push(ev);
}

void EPianoSynth_ControlChange(uint8_t control, uint8_t value)
{
    /* Выбрасываем контроллеры, которые движок не использует, чтобы не занимать FIFO */
    switch (control) {
        case 1: case 7: case 64: case 71: case 72: case 74: case 120: case 123:
            break;
        default:
            return;
    }
    Event ev = { EV_CC, control, (uint8_t)(value > 127 ? 127 : value) };
    fifo_push(ev);
}

void EPianoSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames)
{
    process_events();

    float acc_l[EPIANO_BLOCK];
    float acc_r[EPIANO_BLOCK];

    const float out_scale = EPIANO_MASTER_GAIN * g_volume;

    uint32_t done = 0;
    while (done < num_frames) {
        uint32_t n = num_frames - done;
        if (n > EPIANO_BLOCK) {
            n = EPIANO_BLOCK;
        }

        std::memset(acc_l, 0, n * sizeof(float));
        std::memset(acc_r, 0, n * sizeof(float));

        for (int i = 0; i < EPIANO_NUM_VOICES; i++) {
            if (g_voices[i].active) {
                render_voice(g_voices[i], acc_l, acc_r, n);
            }
        }

        for (uint32_t i = 0; i < n; i++) {
            float l = soft_limit(acc_l[i] * out_scale * 2.0f) * 32767.0f;
            float r = soft_limit(acc_r[i] * out_scale * 2.0f) * 32767.0f;
            buffer[(done + i) * 2]     = (int16_t)(l >= 0.0f ? l + 0.5f : l - 0.5f);
            buffer[(done + i) * 2 + 1] = (int16_t)(r >= 0.0f ? r + 0.5f : r - 0.5f);
        }

        done += n;
    }
}

uint32_t EPianoSynth_GetDroppedEventCount(void)
{
    return g_dropped.load(std::memory_order_relaxed);
}

uint32_t EPianoSynth_GetActiveVoiceCount(void)
{
    uint32_t n = 0;
    for (int i = 0; i < EPIANO_NUM_VOICES; i++) {
        if (g_voices[i].active) n++;
    }
    return n;
}

} // extern "C"

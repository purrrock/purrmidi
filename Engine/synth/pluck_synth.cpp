#include "pluck_synth.h"
#include "synth_arena.h"
#include "synth_common.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

/*
 * Нейлоновая гитарная струна: модель Карплуса-Стронга с собственной реализацией (без DaisySP Pluck).
 *
 * Почему не DaisySP Pluck: он проигрывает буфер шума с шагом, зависящим от высоты ноты, и фильтрует его раз за проход
 * по всему буферу. Из-за этого возбуждение (белый шум) прореживается с алиасингом, а верхние гармоники почти не затухают,
 * и звук получается «металлическим», как у звенящей стальной струны. Здесь всё устроено как у настоящей струны:
 *
 *   1. Возбуждение: шум, пропущенный через два ФНЧ (мягкий палец/ноготь нейлоновой струны, чем сильнее удар, тем ярче),
 *      гребенчатый фильтр положения щипка (гармоники, кратные 1/β, подавлены - как у гитары) и нормировка по RMS.
 *   2. Петля струны: линия задержки + all-pass для точной настройки + однополюсный ФНЧ потерь + усиление < 1.
 *      Усиление задаёт время затухания основного тона (T60), ФНЧ - гораздо более быстрое затухание обертонов:
 *      нейлон «теплеет» уже через десятки миллисекунд после щипка.
 *   3. Корпус: три параллельных резонатора (≈100, 205 и 410 Гц - воздушный резонанс и деки классической гитары),
 *      добавляют телу звука «дерево». Корпус общий на все голоса.
 *
 * Мод-колесо (CC1) только ДОБАВЛЯЕТ яркость к текущей: в нуле (колесо отпущено) тембр и длительность не меняются.
 * Раньше CC1 жёстко задавал демпфирование, и в нуле струна глохла за ~0,2 с.
 */

#define PLUCK_SAMPLE_RATE       48000.0f
#define PLUCK_BUFFER_SIZE       2048                     // степень двойки; минимальная частота SR/2040 ≈ 23,5 Гц
#define PLUCK_BUFFER_MASK       (PLUCK_BUFFER_SIZE - 1)
#define PLUCK_RELEASE_TIME_SEC  0.20f   // Время гашения струны (демпфер) после Note Off, постоянная времени e^-1
#define PLUCK_OUTPUT_GAIN       1.9f    // Общее усиление (подобрано по измерениям RMS относительно E-Piano/Lead/Violin)
#define PLUCK_LIMIT_KNEE        0.85f   // Мягкий ограничитель: ниже колена сигнал не меняется
#define PLUCK_LIMIT_CEIL        0.99f
#define PLUCK_MAX_DAMP          1.0f    // Яркость 0..1
#define PLUCK_REF_FREQ          261.63f // C4: опорная частота для масштабирования потерь по высоте ноты
#define PLUCK_EXCITE_RMS        0.30f   // Целевой RMS возбуждения при velocity 127
#define PLUCK_PICK_POSITION     0.17f   // Положение щипка вдоль струны (доля длины от моста)
#define PLUCK_DECAY_DEFAULT     0.88f
#define PLUCK_BRIGHT_DEFAULT    0.40f
#define EVENT_FIFO_SIZE         32      // Буфер событий NoteOn, NoteOff, CC (степень двойки)
#define NOTE_ON_FIFO_LIMIT      28      // Лимит слотов для Note On (оставляет резерв для Note Off и CC)

static constexpr float kTwoPi = 6.28318530717958647692f;

enum PluckEventType : uint8_t {
    EVENT_NOTE_ON,
    EVENT_NOTE_OFF,
    EVENT_CONTROL_CHANGE
};

struct PluckEvent {
    PluckEventType type;
    uint8_t param1; // note number or CC control
    uint8_t param2; // velocity or CC value
    float freq;
    float amp;
    float damp_offset;
};

struct PluckVoice {
    float line[PLUCK_BUFFER_SIZE];             // Линия задержки струны (кольцевой буфер)
    std::atomic<int32_t> voice_state{-1};      // -1 = свободен, 0..127 = номер активной ноты
    uint32_t write_pos{0};
    int32_t  delay_int{2};                     // Целая часть задержки петли, сэмплы
    float ap_coeff{0.0f}, ap_x1{0.0f}, ap_y1{0.0f};   // All-pass дробной задержки (точная настройка)
    float lp_coeff{0.3f}, lp_state{0.0f};              // ФНЧ потерь в петле
    float loop_gain{0.99f};                    // Усиление петли (задаёт T60 основного тона)
    float c_scale{1.0f};                       // (C4/f)^0.8: потери ФНЧ масштабируются высотой ноты
    float t_scale{1.0f};                       // (C4/f)^0.35: у высоких нот затухание короче
    float damp_offset{0.0f};
    float freq{440.0f};
    float damper_env{0.0f};
    float level_ema{0.0f};
    uint32_t trigger_age{0};
    uint8_t press_count{0};                    // Счётчик удержаний нажатия клавиши для данной ноты
    bool sustain_held{false};
};

// Голоса лежат в общей арене памяти (synth_arena.h): одновременно звучит один движок. Указатель устанавливается
// в PluckSynth_Init(); если арену забрал другой движок, SynthArena_IsOwner(&voices) == false.
static PluckVoice* voices = nullptr;

static uint32_t global_trigger_counter = 0;
static float release_coeff = 0.0f;
static uint32_t noise_state = 0x1234567u;   // xorshift32; сбрасывается в Init(): звук воспроизводим бит-в-бит

// Переменные состояния непрерывных контроллеров (MIDI CC)
static std::atomic<float> user_decay{PLUCK_DECAY_DEFAULT};
static std::atomic<float> user_damp{PLUCK_BRIGHT_DEFAULT};   // яркость 0..1 (выше - ярче)
static std::atomic<float> user_mod{0.0f};                    // мод-колесо 0..1: прибавка яркости, в нуле нейтрально

// Параметры, с которыми последний раз пересчитывались коэффициенты звучащих голосов (только аудио-контекст)
static float applied_decay = -1.0f, applied_damp = -1.0f, applied_mod = -1.0f;

// Очередь событий, учёт нажатий и протокол аварийного восстановления (общий компонент, см. synth_common.h)
static synth::NoteInput<PluckEvent, EVENT_FIFO_SIZE, NOTE_ON_FIFO_LIMIT> input;
static bool audio_sustain_pedal = false;

static inline float clamp_f(float val, float min_val, float max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

static inline float next_noise() {   // -1..1
    uint32_t x = noise_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    noise_state = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Корпус гитары: параллельные резонаторы-полосовые фильтры (RBJ, 0 дБ в пике), общие на все голоса
// ---------------------------------------------------------------------------------------------------------------------
struct BodyMode {
    float freq, q, gain;
    float b0, a1, a2;      // b1 = 0, b2 = -b0
    float z1, z2;
};

static BodyMode body[3] = {
    {  98.0f, 5.0f, 0.45f, 0, 0, 0, 0, 0 },   // воздушный резонанс (Гельмгольц)
    { 205.0f, 7.0f, 0.55f, 0, 0, 0, 0, 0 },   // основной резонанс верхней деки
    { 410.0f, 6.0f, 0.35f, 0, 0, 0, 0, 0 },   // вторая мода деки
};

static void body_init() {
    for (BodyMode& m : body) {
        const float w0    = kTwoPi * m.freq / PLUCK_SAMPLE_RATE;
        const float alpha = std::sin(w0) / (2.0f * m.q);
        const float a0    = 1.0f + alpha;
        m.b0 = alpha / a0;
        m.a1 = -2.0f * std::cos(w0) / a0;
        m.a2 = (1.0f - alpha) / a0;
        m.z1 = m.z2 = 0.0f;
    }
}

static inline float body_process(float x) {
    float y = x;
    for (BodyMode& m : body) {
        const float bp = m.b0 * x + m.z1;
        m.z1 = m.z2 - m.a1 * bp;           // b1 = 0
        m.z2 = -m.b0 * x - m.a2 * bp;      // b2 = -b0
        y += m.gain * bp;
    }
    return y;
}

// Корпус «затих»: состояния настолько малы, что выход гарантированно округляется до нуля (int16)
static inline bool body_is_silent() {
    float e = 0.0f;
    for (const BodyMode& m : body) e += std::abs(m.z1) + std::abs(m.z2);
    return e < 1e-7f;
}

static inline void body_reset_state() {
    for (BodyMode& m : body) m.z1 = m.z2 = 0.0f;
}

// ---------------------------------------------------------------------------------------------------------------------
// Параметры петли струны
// ---------------------------------------------------------------------------------------------------------------------

// Яркость 0..1 с учётом мод-колеса: колесо только прибавляет, в нуле ничего не меняется
static inline float effective_brightness(float damp, float mod, float offset) {
    return clamp_f(damp + offset + (1.0f - damp) * mod, 0.0f, 1.0f);
}

// Коэффициент ФНЧ потерь и усиление петли. Вызывается при Note On и при изменении контроллеров.
// Настройку (задержку) здесь не трогаем: она считается один раз в начале ноты, поэтому без щелчков.
static void update_loop(PluckVoice& v, float decay, float brightness) {
    // ФНЧ потерь: чем ярче, тем слабее; масштабируется по высоте (на высоких нотах проходов за секунду больше)
    float c = (0.05f + 0.55f * (1.0f - brightness)) * v.c_scale;
    v.lp_coeff = clamp_f(c, 0.02f, 0.62f);

    // Время затухания основного тона на C4: 0.5 -> 1.2 с, 0.88 -> 3.1 с, 1.0 -> 3.7 с
    float t60 = clamp_f(5.0f * decay - 1.3f, 0.15f, 3.7f) * v.t_scale;
    t60 = clamp_f(t60, 0.15f, 8.0f);
    // gain^(f*T60) = 10^-3 (-60 дБ): за T60 секунд происходит f*T60 проходов по петле
    const float g = std::exp(-6.907755f / (v.freq * t60));
    v.loop_gain = (g > 0.99995f) ? 0.99995f : g;
}

// Возбуждение струны: мягкий щипок. Заполняет линию задержки и сбрасывает состояния фильтров петли.
static void excite(PluckVoice& v, float amp, float norm_vel, float brightness) {
    const int32_t n = v.delay_int;
    float* e = v.line;

    // Частота среза возбуждения: ярче при сильном ударе и при большей яркости (нейлон + подушечка пальца)
    const float fc = 500.0f + 3000.0f * brightness + 1500.0f * norm_vel;
    const float a  = std::exp(-kTwoPi * fc / PLUCK_SAMPLE_RATE);
    float y1 = 0.0f, y2 = 0.0f;
    for (int32_t i = 0; i < n; i++) {
        y1 = (1.0f - a) * next_noise() + a * y1;   // два однополюсных ФНЧ: -12 дБ/окт
        y2 = (1.0f - a) * y1 + a * y2;
        e[i] = y2;
    }

    // Положение щипка: e[j] -= e[j - P]; идём с конца, чтобы использовать ещё не изменённые значения
    int32_t pick = (int32_t)(PLUCK_PICK_POSITION * (float)n + 0.5f);
    if (pick < 1) pick = 1;
    for (int32_t j = n - 1; j >= pick; j--) {
        e[j] -= e[j - pick];
    }

    // Убираем постоянную составляющую (иначе она бы звучала как щелчок и «смещала» струну) и нормируем по RMS
    float mean = 0.0f;
    for (int32_t i = 0; i < n; i++) mean += e[i];
    mean /= (float)n;
    float energy = 0.0f;
    for (int32_t i = 0; i < n; i++) {
        e[i] -= mean;
        energy += e[i] * e[i];
    }
    const float rms   = std::sqrt(energy / (float)n);
    const float scale = (rms > 1e-9f) ? (PLUCK_EXCITE_RMS * amp / rms) : 0.0f;
    for (int32_t i = 0; i < n; i++) e[i] *= scale;

    v.write_pos = (uint32_t)n & PLUCK_BUFFER_MASK;   // первое чтение: line[0]
    v.ap_x1 = v.ap_y1 = 0.0f;
    v.lp_state = 0.0f;
}

// Один сэмпл петли струны
static inline float process_voice(PluckVoice& v) {
    const uint32_t read_pos = (v.write_pos - (uint32_t)v.delay_int) & PLUCK_BUFFER_MASK;
    const float s  = v.line[read_pos];

    const float ap = v.ap_coeff * s + v.ap_x1 - v.ap_coeff * v.ap_y1;   // all-pass: дробная часть задержки
    v.ap_x1 = s;
    v.ap_y1 = ap;

    v.lp_state = (1.0f - v.lp_coeff) * ap + v.lp_coeff * v.lp_state;    // потери на высоких частотах
    const float fb = v.loop_gain * v.lp_state;

    v.line[v.write_pos] = fb;
    v.write_pos = (v.write_pos + 1U) & PLUCK_BUFFER_MASK;
    return fb;
}

void PluckSynth_Init(void) {
    // Голоса создаются в общей арене заново: данные прежнего владельца (другого движка) стираются.
    voices = SynthArena_NewArray<PluckVoice, PLUCK_VOICES>(&voices);

    release_coeff = std::exp(-1.0f / (PLUCK_SAMPLE_RATE * PLUCK_RELEASE_TIME_SEC));
    global_trigger_counter = 0;
    audio_sustain_pedal = false;
    noise_state = 0x1234567u;
    applied_decay = applied_damp = applied_mod = -1.0f;

    for (int i = 0; i < PLUCK_VOICES; i++) {
        std::memset(voices[i].line, 0, sizeof(voices[i].line));
        voices[i].voice_state.store(-1, std::memory_order_relaxed);
        voices[i].write_pos    = 0;
        voices[i].delay_int    = 2;
        voices[i].ap_coeff     = 0.0f;
        voices[i].ap_x1        = voices[i].ap_y1 = 0.0f;
        voices[i].lp_coeff     = 0.3f;
        voices[i].lp_state     = 0.0f;
        voices[i].loop_gain    = 0.99f;
        voices[i].c_scale      = 1.0f;
        voices[i].t_scale      = 1.0f;
        voices[i].damp_offset  = 0.0f;
        voices[i].freq         = 440.0f;
        voices[i].damper_env   = 0.0f;
        voices[i].level_ema    = 0.0f;
        voices[i].trigger_age  = 0;
        voices[i].press_count  = 0;
        voices[i].sustain_held = false;
    }

    body_init();

    user_decay.store(PLUCK_DECAY_DEFAULT, std::memory_order_relaxed);
    user_damp.store(PLUCK_BRIGHT_DEFAULT, std::memory_order_relaxed);
    user_mod.store(0.0f, std::memory_order_relaxed);

    input.Reset();
}

void PluckSynth_NoteOn(uint8_t midi_note, uint8_t velocity) {
    if (velocity == 0) {
        PluckSynth_NoteOff(midi_note);
        return;
    }

    if (midi_note >= 128) return;

    float freq = 440.0f * std::pow(2.0f, ((float)midi_note - 69.0f) * (1.0f / 12.0f));
    float min_freq = (PLUCK_SAMPLE_RATE / (float)(PLUCK_BUFFER_SIZE - 4));
    float max_freq = (PLUCK_SAMPLE_RATE * 0.45f);
    freq = clamp_f(freq, min_freq, max_freq);

    float norm_vel = clamp_f((float)velocity * (1.0f / 127.0f), 0.0f, 1.0f);
    float amp = 0.05f + 0.95f * norm_vel;

    float damp_offset = norm_vel * 0.10f;

    PluckEvent event;
    event.type        = EVENT_NOTE_ON;
    event.param1      = midi_note;
    event.param2      = velocity;
    event.freq        = freq;
    event.amp         = amp;
    event.damp_offset = damp_offset;

    input.NoteOn(midi_note, event);
}

void PluckSynth_NoteOff(uint8_t midi_note) {
    if (midi_note >= 128) return;

    PluckEvent event{};
    event.type   = EVENT_NOTE_OFF;
    event.param1 = midi_note;
    event.param2 = 0;

    input.NoteOff(midi_note, event);
}

void PluckSynth_SetDecay(float decay) {
    user_decay.store(clamp_f(decay, 0.0f, 1.0f), std::memory_order_relaxed);
}

void PluckSynth_SetDamp(float damp) {
    user_damp.store(clamp_f(damp, 0.0f, PLUCK_MAX_DAMP), std::memory_order_relaxed);
}

bool PluckSynth_IsNoteActive(uint8_t midi_note) {
    if (midi_note >= 128 || !SynthArena_IsOwner(&voices)) return false;
    for (int i = 0; i < PLUCK_VOICES; i++) {
        if (voices[i].voice_state.load(std::memory_order_acquire) == (int32_t)midi_note) {
            return true;
        }
    }
    return false;
}

uint32_t PluckSynth_GetDroppedEventsCount(void) {
    return input.Dropped();
}

void PluckSynth_ControlChange(uint8_t control, uint8_t value) {
    float norm = (float)value * (1.0f / 127.0f);

    switch (control) {
        case 1:   // Мод-колесо: прибавка яркости к текущей; в нуле (колесо отпущено) тембр не меняется
            user_mod.store(norm, std::memory_order_relaxed);
            break;

        case 71:  // Яркость (абсолютная)
        case 74:
            PluckSynth_SetDamp(0.10f + norm * 0.90f);
            break;

        case 72: 
            PluckSynth_SetDecay(0.50f + norm * 0.495f);
            break;

        case 64: {
            PluckEvent event{};
            event.type   = EVENT_CONTROL_CHANGE;
            event.param1 = control;
            event.param2 = value;
            input.Critical(event);
            break;
        }

        default:
            break;
    }
}

int16_t PluckSynth_NextSample(void) {
    if (!SynthArena_IsOwner(&voices)) {     // арену забрал другой движок или Init() ещё не вызывался
        return 0;
    }

    // Протокол аварийного восстановления (см. synth_common.h, NoteInput::ServiceRecovery): когда ни один производитель
    // не находится в критической секции, сбрасываем педаль и удержания нажатий, счётчики и очередь событий.
    input.ServiceRecovery([]() {
        audio_sustain_pedal = false;

        for (int i = 0; i < PLUCK_VOICES; i++) {
            voices[i].press_count  = 0;
            voices[i].sustain_held = false;
        }
    });

    PluckEvent event;

    // 1. Последовательная строго хронологическая обработка событий из FIFO
    while (input.Pop(event)) {
        if (event.type == EVENT_NOTE_ON) {
            int target_voice = -1;

            // Поиск голоса, уже играющего данную ноту
            for (int i = 0; i < PLUCK_VOICES; i++) {
                if (voices[i].voice_state.load(std::memory_order_relaxed) == (int32_t)event.param1) {
                    target_voice = i;
                    break;
                }
            }

            // Иначе ищем свободный голос
            if (target_voice == -1) {
                for (int i = 0; i < PLUCK_VOICES; i++) {
                    if (voices[i].voice_state.load(std::memory_order_relaxed) == -1) {
                        target_voice = i;
                        break;
                    }
                }
            }

            // Иначе вытесняем самый старый голос
            if (target_voice == -1) {
                uint32_t max_age_diff = 0;
                target_voice = 0;
                for (int i = 0; i < PLUCK_VOICES; i++) {
                    uint32_t age_diff = global_trigger_counter - voices[i].trigger_age;
                    if (age_diff >= max_age_diff) {
                        max_age_diff = age_diff;
                        target_voice = i;
                    }
                }
            }

            PluckVoice& v = voices[target_voice];
            v.freq         = event.freq;
            v.damp_offset  = event.damp_offset;
            v.damper_env   = 1.0f;
            v.level_ema    = 1.0f;
            v.trigger_age  = ++global_trigger_counter;
            if (v.press_count < 255) v.press_count++;
            v.sustain_held = false;

            const float ratio = PLUCK_REF_FREQ / v.freq;
            v.c_scale = std::pow(ratio, 0.8f);
            v.t_scale = std::pow(ratio, 0.35f);

            const float cur_decay = user_decay.load(std::memory_order_relaxed);
            const float bright = effective_brightness(user_damp.load(std::memory_order_relaxed),
                                                      user_mod.load(std::memory_order_relaxed), v.damp_offset);
            update_loop(v, cur_decay, bright);

            // Настройка: период = SR/f; из него вычитаем задержку ФНЧ на низких частотах c/(1-c);
            // остаток делим на целую часть и дробную d в [0.5, 1.5) - её отрабатывает all-pass
            float total = PLUCK_SAMPLE_RATE / v.freq - v.lp_coeff / (1.0f - v.lp_coeff);
            total = clamp_f(total, 1.6f, (float)(PLUCK_BUFFER_SIZE - 4));
            v.delay_int = (int32_t)(total - 0.5f);
            const float d = total - (float)v.delay_int;
            v.ap_coeff = (1.0f - d) / (1.0f + d);

            const float norm_vel = clamp_f((float)event.param2 * (1.0f / 127.0f), 0.0f, 1.0f);
            excite(v, event.amp, norm_vel, bright);

            v.voice_state.store((int32_t)event.param1, std::memory_order_release);

        } else if (event.type == EVENT_NOTE_OFF) {
            uint8_t note = event.param1;
            for (int i = 0; i < PLUCK_VOICES; i++) {
                if (voices[i].voice_state.load(std::memory_order_relaxed) == (int32_t)note && voices[i].press_count > 0) {
                    voices[i].press_count--;
                    if (voices[i].press_count == 0) {
                        if (audio_sustain_pedal) {
                            voices[i].sustain_held = true; // Захват педалью ТОЛЬКО если педаль была нажата
                        } else {
                            voices[i].sustain_held = false;
                        }
                    }
                }
            }
        } else if (event.type == EVENT_CONTROL_CHANGE) {
            if (event.param1 == 64) {
                bool pedal_on = (event.param2 >= 64);
                audio_sustain_pedal = pedal_on;
                if (!pedal_on) {
                    // Отпускание педали снимает удержание у всех голосов
                    for (int i = 0; i < PLUCK_VOICES; i++) {
                        voices[i].sustain_held = false;
                    }
                }
            }
        }
    }

    // 2. Вычисление и смешивание сигналов активных голосов
    float mix_sum_f = 0.0f;
    uint32_t active_voices_count = 0;
    const float current_decay = user_decay.load(std::memory_order_relaxed);
    const float current_damp  = user_damp.load(std::memory_order_relaxed);
    const float current_mod   = user_mod.load(std::memory_order_relaxed);

    // Контроллеры изменились: пересчитываем потери и затухание звучащих голосов (настройка не меняется)
    if (current_decay != applied_decay || current_damp != applied_damp || current_mod != applied_mod) {
        applied_decay = current_decay;
        applied_damp  = current_damp;
        applied_mod   = current_mod;
        for (int i = 0; i < PLUCK_VOICES; i++) {
            if (voices[i].voice_state.load(std::memory_order_relaxed) >= 0) {
                update_loop(voices[i], current_decay,
                            effective_brightness(current_damp, current_mod, voices[i].damp_offset));
            }
        }
    }

    for (int i = 0; i < PLUCK_VOICES; i++) {
        PluckVoice& v = voices[i];

        if (v.voice_state.load(std::memory_order_relaxed) >= 0) {
            float sample_f = process_voice(v);

            // Если клавиша отпущена И нота не удерживается педалью CC64, опускаем демпфер (damper decay)
            if (v.press_count == 0 && !v.sustain_held) {
                v.damper_env *= release_coeff;
            }

            sample_f *= v.damper_env;

            // Оценка сглаженного уровня энергии (EMA)
            v.level_ema = 0.99f * v.level_ema + 0.01f * std::abs(sample_f);

            if (v.level_ema < 0.0001f) {
                v.voice_state.store(-1, std::memory_order_release);
                v.press_count  = 0;
                v.sustain_held = false;
                sample_f       = 0.0f;
            } else {
                active_voices_count++;
                mix_sum_f += sample_f;
            }
        }
    }

    // Динамическая нормализация при суммировании нескольких голосов
    float norm_gain = 1.0f;
    if (active_voices_count > 1) {
        norm_gain = 1.0f / (1.0f + 0.40f * (float)(active_voices_count - 1));
    }

    // Корпус гитары. Когда все голоса замолчали, даём резонаторам дозвучать и затем обнуляем их состояние:
    // тишина должна быть точной (без микроскопических остатков).
    float body_out = body_process(mix_sum_f * norm_gain);
    if (active_voices_count == 0 && body_is_silent()) {
        body_reset_state();
        body_out = 0.0f;
    }

    const float limited = synth::SoftLimit(body_out * PLUCK_OUTPUT_GAIN, PLUCK_LIMIT_KNEE, PLUCK_LIMIT_CEIL);
    return (int16_t)(limited * 32767.0f);
}

void PluckSynth_FillStereoBuffer(int16_t *buffer, uint32_t num_frames) {
    synth::EnableFlushToZero();   // денормали замедляют затухающие хвосты (см. synth_common.h)
    for (uint32_t i = 0; i < num_frames; i++) {
        int16_t sample = PluckSynth_NextSample();
        buffer[i * 2]     = sample; // L
        buffer[i * 2 + 1] = sample; // R
    }
}
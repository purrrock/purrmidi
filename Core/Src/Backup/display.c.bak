#include "display.h"
#include "chord_detector.h"

#include "main.h"
#include "stm32h7xx_hal.h"

#include "st7735.h"
#include "font.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Hardware                                                                  */
/* ------------------------------------------------------------------------- */

#define DISPLAY_WIDTH   160U
#define DISPLAY_HEIGHT   80U

#define LCD_CS_PORT      GPIOE
#define LCD_CS_PIN       GPIO_PIN_11

#define LCD_DC_PORT      GPIOE
#define LCD_DC_PIN       GPIO_PIN_13

#define LCD_BL_PORT      GPIOE
#define LCD_BL_PIN       GPIO_PIN_10

/* Подсветка на этой плате включается низким уровнем */
#define LCD_BL_ON        GPIO_PIN_RESET
#define LCD_BL_OFF       GPIO_PIN_SET

extern SPI_HandleTypeDef hspi4;

static ST7735_Object_t st7735;
static ST7735_Ctx_t st7735_ctx;
static bool display_ready = false;   /* true только после успешной инициализации */

/* ------------------------------------------------------------------------- */
/* Layout / palette                                                          */
/* ------------------------------------------------------------------------- */

#define HDR_Y        0U
#define HDR_H       14U
#define INS_Y       16U
#define INS_H       16U
#define NOTE_Y      34U
#define NOTE_H      32U
#define VOL_Y       67U
#define VOL_H       13U

#define DOT_X        3U
#define DOT_Y        3U
#define DOT_SIZE     7U

#define MIN_DRAW_INTERVAL_MS   10U   /* пауза между отрисовками полей        */
#define ERROR_TIMEOUT_MS     3000U   /* время показа "временной" ошибки      */
#define ENUM_TIMEOUT_MS      5000U   /* энумерация дольше этого -> E01       */

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | (((b) & 0xFFU) >> 3)))

#define C_BLACK     RGB565(  0,   0,   0)
#define C_WHITE     RGB565(240, 240, 240)
#define C_DIM       RGB565(105, 110, 125)
#define C_GRAY      RGB565( 90,  90, 100)
#define C_IDLE      RGB565( 70,  70,  80)
#define C_HDR       RGB565( 22,  27,  42)
#define C_CYAN      RGB565(  0, 200, 230)
#define C_AMBER     RGB565(255, 190,  40)
#define C_MINT      RGB565(120, 255, 160)
#define C_RED       RGB565(225,  45,  45)
#define C_GREEN     RGB565( 40, 220,  80)
#define C_YELLOW    RGB565(240, 210,  40)

/* Поля, которые можно перерисовать независимо */
#define F_HEADER    (1U << 0)
#define F_INSTR     (1U << 2)
#define F_NOTE      (1U << 3)
#define F_VOLUME    (1U << 4)
#define F_ALL       (F_HEADER | F_INSTR | F_NOTE | F_VOLUME)

/* ------------------------------------------------------------------------- */
/* UI state                                                                  */
/* ------------------------------------------------------------------------- */

typedef enum { NOTE_KIND_NONE = 0, NOTE_KIND_SINGLE, NOTE_KIND_CHORD } NoteKind_t;

typedef struct
{
    bool     active;
    uint32_t expire;      /* tick, когда временная ошибка гаснет */
    uint32_t stamp;       /* порядковый номер появления          */
} ErrorSlot_t;

static const struct
{
    const char *text;
    bool        sticky;
} error_info[DISPLAY_ERR_COUNT] =
{
    [DISPLAY_ERR_NONE]       = { "",                 false },
    [DISPLAY_ERR_USB_ENUM]   = { "USB ENUM FAIL",    true  },
    [DISPLAY_ERR_AUDIO_DMA]  = { "AUDIO DMA ERROR",  false },
    [DISPLAY_ERR_MIDI_QUEUE] = { "MIDI QUEUE FULL",  false },
    [DISPLAY_ERR_USB_RX]     = { "USB RX ERROR",     false },
    [DISPLAY_ERR_AUDIO_OVERLOAD] = { "AUDIO OVERLOAD", false },
    [DISPLAY_ERR_SF2]        = { "SF2 NOT LOADED",   false },
};

static struct
{
    Display_Link_t link;
    uint32_t       link_since;       /* tick последней смены состояния линка */
    char           device[13];

    NoteKind_t     kind;
    bool           note_live;        /* клавиши сейчас нажаты; иначе - серая "последняя" */
    uint8_t        note;
    uint16_t       key_count;
    char           chord[16];
    uint8_t        velocity;
    bool           sustain;

    uint8_t        patch_index;
    char           patch_name[18];

    uint8_t        voices_used;
    uint8_t        voices_max;

    bool           volume_valid;
    uint8_t        volume;
    bool           clip;

    ErrorSlot_t    errors[DISPLAY_ERR_COUNT];
    uint32_t       error_stamp;

    uint32_t       dirty;
    uint32_t       last_draw_tick;
} ui =
{
    .link        = DISPLAY_LINK_NONE,
    .patch_index = 1,
    .patch_name  = "Pluck",
    .dirty       = F_ALL,
};

static const char * const note_names[12] =
{
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

/* ------------------------------------------------------------------------- */
/* ST7735 bus interface                                                      */
/* ------------------------------------------------------------------------- */

static int32_t Display_BusInit(void)
{
    return ST7735_OK;
}

static int32_t Display_GetTick(void)
{
    return (int32_t)HAL_GetTick();
}

static int32_t Display_WriteReg(
    uint8_t reg,
    uint8_t *data,
    uint32_t length)
{
    int32_t result;

    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_RESET);

    result = HAL_SPI_Transmit(&hspi4, &reg, 1, 100);

    if (result == HAL_OK && length > 0U)
    {
        HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_SET);
        result = HAL_SPI_Transmit(&hspi4, data, length, 500);
    }

    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_SET);

    return (result == HAL_OK) ? ST7735_OK : ST7735_ERROR;
}

static int32_t Display_SendData(
    uint8_t *data,
    uint32_t length)
{
    int32_t result;

    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_SET);

    result = HAL_SPI_Transmit(&hspi4, data, length, 500);

    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_SET);

    return (result == HAL_OK) ? ST7735_OK : ST7735_ERROR;
}

static ST7735_IO_t st7735_io =
{
    .Init     = Display_BusInit,
    .DeInit   = NULL,
    .Address  = 0,
    .WriteReg = Display_WriteReg,
    .ReadReg  = NULL, // Чтение недоступно
    .SendData = Display_SendData,
    .RecvData = NULL, // Чтение недоступно
    .GetTick  = Display_GetTick
};

/* ------------------------------------------------------------------------- */
/* Canvas: поле рисуется целиком в RAM и уходит на экран одним блитом        */
/* (нет мерцания "стёрли -> нарисовали", меньше SPI-команд)                  */
/* ------------------------------------------------------------------------- */

#define CANVAS_MAX_W   DISPLAY_WIDTH
#define CANVAS_MAX_H   32U

static uint16_t canvas[CANVAS_MAX_W * CANVAS_MAX_H];
static uint16_t canvas_w;
static uint16_t canvas_h;

/*
 * ST7735_FillRGBRect() отправляет байты в порядке памяти (на Cortex-M это
 * младший байт первым), а панель ждёт старший первым. Поэтому в буфере
 * хранится цвет с переставленными байтами. (FillRect/DrawHLine меняют байты
 * сами - с ними такой проблемы нет.)
 */
static inline uint16_t Swap16(uint16_t c)
{
    return (uint16_t)((c << 8) | (c >> 8));
}

static void Canvas_Begin(uint16_t w, uint16_t h, uint16_t bg)
{
    canvas_w = w;
    canvas_h = h;

    const uint16_t v = Swap16(bg);
    for (uint32_t i = 0; i < (uint32_t)w * h; i++)
    {
        canvas[i] = v;
    }
}

static void Canvas_Rect(int x, int y, int w, int h, uint16_t color)
{
    const uint16_t v = Swap16(color);

    for (int yy = y; yy < y + h; yy++)
    {
        if (yy < 0 || yy >= (int)canvas_h) { continue; }

        for (int xx = x; xx < x + w; xx++)
        {
            if (xx >= 0 && xx < (int)canvas_w)
            {
                canvas[yy * canvas_w + xx] = v;
            }
        }
    }
}

/*
 * Формат шрифта asc2_1206 / asc2_1608: глиф хранится по столбцам слева
 * направо, каждый столбец занимает (size + 7) / 8 байт, внутри байта
 * старший бит - верхняя строка. Рисуются только "включённые" пиксели,
 * фон уже залит Canvas_Begin().
 */
static void Canvas_Char(int x, int y, char c, uint8_t size, uint8_t scale, uint16_t color)
{
    if (c < ' ' || c > '~' || (size != 12U && size != 16U))
    {
        return;
    }

    const uint8_t width     = size / 2U;
    const uint8_t col_bytes = (size + 7U) / 8U;
    const uint8_t *glyph    = (size == 12U) ? asc2_1206[c - ' ']
                                            : asc2_1608[c - ' '];

    for (uint8_t col = 0; col < width; col++)
    {
        for (uint8_t row = 0; row < size; row++)
        {
            const uint8_t bits = glyph[col * col_bytes + (row >> 3)];

            if (bits & (0x80U >> (row & 7U)))
            {
                Canvas_Rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

/* Возвращает X сразу за последним символом */
static int Canvas_Text(int x, int y, const char *text, uint8_t size, uint8_t scale, uint16_t color)
{
    for (; *text != '\0'; text++)
    {
        Canvas_Char(x, y, *text, size, scale, color);
        x += (size / 2) * scale;
    }
    return x;
}

static void Canvas_Flush(uint16_t x, uint16_t y)
{
    if (!display_ready) { return; }

    ST7735_FillRGBRect(&st7735, x, y, (uint8_t *)canvas, canvas_w, canvas_h);
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static void CopyText(char *dst, size_t dst_size, const char *src)
{
    if (src == NULL) { src = ""; }

    size_t i = 0;
    for (; i + 1 < dst_size && src[i] != '\0'; i++)
    {
        /* шрифт только ASCII */
        dst[i] = (src[i] >= ' ' && src[i] <= '~') ? src[i] : '?';
    }
    dst[i] = '\0';
}

static void MarkDirty(uint32_t fields)
{
    ui.dirty |= fields;
}

/* Какую ошибку показывать: липкая важнее временной, среди равных - более новая */
static Display_Error_t Error_Pick(uint8_t *extra)
{
    Display_Error_t best = DISPLAY_ERR_NONE;
    uint8_t count = 0;

    for (uint8_t i = 1; i < DISPLAY_ERR_COUNT; i++)
    {
        if (!ui.errors[i].active) { continue; }
        count++;

        if (best == DISPLAY_ERR_NONE)
        {
            best = (Display_Error_t)i;
            continue;
        }

        const bool best_sticky = error_info[best].sticky;
        const bool cur_sticky  = error_info[i].sticky;

        if ((cur_sticky && !best_sticky) ||
            (cur_sticky == best_sticky && ui.errors[i].stamp > ui.errors[best].stamp))
        {
            best = (Display_Error_t)i;
        }
    }

    if (extra != NULL) { *extra = (count > 0) ? (uint8_t)(count - 1) : 0; }
    return best;
}

/* ------------------------------------------------------------------------- */
/* Field renderers                                                           */
/* ------------------------------------------------------------------------- */

static uint16_t Dot_Color(void)
{
    if (ui.errors[DISPLAY_ERR_USB_ENUM].active) { return C_RED; }

    switch (ui.link)
    {
        case DISPLAY_LINK_READY:
            return C_GREEN;
        case DISPLAY_LINK_ENUMERATING:
            return C_YELLOW;
        default:
            return C_GRAY;
    }
}

static void Canvas_Dot(int x, int y, uint16_t color)
{
    /* Круг 7x7; бит 6 - левый столбец */
    static const uint8_t mask[DOT_SIZE] = { 0x1C, 0x3E, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C };

    for (int row = 0; row < (int)DOT_SIZE; row++)
    {
        for (int col = 0; col < (int)DOT_SIZE; col++)
        {
            if (mask[row] & (0x40U >> col))
            {
                Canvas_Rect(x + col, y + row, 1, 1, color);
            }
        }
    }
}

static void Draw_Header(void)
{
    uint8_t extra = 0;
    const Display_Error_t err = Error_Pick(&extra);

    if (err != DISPLAY_ERR_NONE)
    {
        /* Красная плашка вместо шапки: "! E03 MIDI QUEUE FULL +1" (до 26 символов) */
        char text[32];
        snprintf(text, sizeof(text), "! E%02u %s", (unsigned)err, error_info[err].text);

        if (extra > 0)
        {
            size_t len = strlen(text);
            if (len > 22) { len = 22; }
            snprintf(&text[len], sizeof(text) - len, " +%u", (unsigned)extra);
        }
        text[26] = '\0';

        Canvas_Begin(DISPLAY_WIDTH, HDR_H, C_RED);
        Canvas_Text(2, 1, text, 12, 1, C_WHITE);
        Canvas_Flush(0, HDR_Y);
        return;
    }

    Canvas_Begin(DISPLAY_WIDTH, HDR_H, C_HDR);
    Canvas_Dot(DOT_X, DOT_Y, Dot_Color());

    const char *name;
    uint16_t name_color = C_DIM;

    switch (ui.link)
    {
        case DISPLAY_LINK_READY:
            name = (ui.device[0] != '\0') ? ui.device : "USB MIDI";
            name_color = C_WHITE;
            break;
        case DISPLAY_LINK_ENUMERATING:
            name = "connecting...";
            break;
        default:
            name = "no keyboard";
            break;
    }
    Canvas_Text(14, 1, name, 12, 1, name_color);

    /* Правая часть: [SUS] [V used/max], выравнивание по правому краю */
    int right = (int)DISPLAY_WIDTH - 2;

    if (ui.voices_max > 0)
    {
        char voices[10];
        snprintf(voices, sizeof(voices), "V%u/%u", (unsigned)ui.voices_used, (unsigned)ui.voices_max);
        right -= (int)strlen(voices) * 6;
        Canvas_Text(right, 1, voices, 12, 1, C_DIM);
        right -= 6;
    }

    if (ui.sustain)
    {
        right -= 18;
        Canvas_Text(right, 1, "SUS", 12, 1, C_AMBER);
    }

    Canvas_Flush(0, HDR_Y);
}

static void Draw_Instrument(void)
{
    char number[4];
    snprintf(number, sizeof(number), "%02u", (unsigned)(ui.patch_index % 100U));

    Canvas_Begin(DISPLAY_WIDTH, INS_H, C_BLACK);
    Canvas_Text(2, 0, number, 16, 1, C_DIM);
    Canvas_Text(24, 0, ui.patch_name, 16, 1, C_CYAN);
    Canvas_Flush(0, INS_Y);
}

static void Draw_Note(void)
{
    Canvas_Begin(DISPLAY_WIDTH, NOTE_H, C_BLACK);

    if (ui.kind != NOTE_KIND_NONE)
    {
        char text[16];
        uint16_t color;

        if (ui.kind == NOTE_KIND_SINGLE)
        {
            /* Нота 0..11 -> октава -1, поэтому знаковый int */
            const int octave = (int)(ui.note / 12U) - 1;
            snprintf(text, sizeof(text), "%s%d", note_names[ui.note % 12U], octave);
            color = ui.note_live ? C_AMBER : C_IDLE;
        }
        else
        {
            snprintf(text, sizeof(text), "%s", ui.chord);
            color = ui.note_live ? C_MINT : C_IDLE;
        }

        const int len = (int)strlen(text);

        /* Крупно (16x32), если помещается в 160 px, иначе обычным 8x16 */
        const uint8_t scale = (len * 16 + 2 <= (int)DISPLAY_WIDTH) ? 2 : 1;
        const int y = (scale == 2) ? 0 : 8;
        const int end_x = Canvas_Text(2, y, text, 16, scale, color);

        char side[12];

        if (ui.kind == NOTE_KIND_SINGLE && ui.note_live)
        {
            snprintf(side, sizeof(side), "NOTE %3u", (unsigned)ui.note);
            Canvas_Text(70, 1, side, 12, 1, C_DIM);

            if (ui.velocity > 0)
            {
                snprintf(side, sizeof(side), "VEL  %3u", (unsigned)ui.velocity);
                Canvas_Text(70, 16, side, 12, 1, C_DIM);
            }
        }
        else if (end_x + 4 <= 124)
        {
            if (ui.note_live)
            {
                snprintf(side, sizeof(side), "x%u", (unsigned)ui.key_count);
                Canvas_Text(124, 18, side, 12, 1, C_DIM);
            }
            else
            {
                Canvas_Text(124, 18, "last", 12, 1, C_GRAY);
            }
        }
    }

    Canvas_Flush(0, NOTE_Y);
}

static void Draw_Volume(void)
{
    Canvas_Begin(DISPLAY_WIDTH, VOL_H, C_BLACK);

    Canvas_Text(2, 0, "VOL", 12, 1, C_DIM);

    /* Рамка 102x10 и внутренняя область 100x8 */
    Canvas_Rect(26, 2, 102, 10, C_GRAY);
    Canvas_Rect(27, 3, 100, 8, C_BLACK);

    char text[8];

    if (ui.volume_valid)
    {
        Canvas_Rect(27, 3, ui.volume, 8, ui.clip ? C_RED : C_GREEN);
        snprintf(text, sizeof(text), "%3u%%", (unsigned)ui.volume);
        Canvas_Text(132, 0, text, 12, 1, C_WHITE);
    }
    else
    {
        Canvas_Text(132, 0, " --%", 12, 1, C_DIM);
    }

    Canvas_Flush(0, VOL_Y);
}

/* Рисует одно "грязное" поле; возвращает true, если что-то нарисовано */
static bool Draw_NextField(void)
{
    if (ui.dirty & F_HEADER)
    {
        ui.dirty &= ~F_HEADER;
        Draw_Header();
    }
    else if (ui.dirty & F_NOTE)
    {
        ui.dirty &= ~F_NOTE;
        Draw_Note();
    }
    else if (ui.dirty & F_INSTR)
    {
        ui.dirty &= ~F_INSTR;
        Draw_Instrument();
    }
    else if (ui.dirty & F_VOLUME)
    {
        ui.dirty &= ~F_VOLUME;
        Draw_Volume();
    }
    else
    {
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* Public API: lifecycle                                                     */
/* ------------------------------------------------------------------------- */

void Display_Init(void)
{
    /* Пока в памяти дисплея мусор, подсветка должна быть выключена */
    HAL_GPIO_WritePin(LCD_BL_PORT, LCD_BL_PIN, LCD_BL_OFF);

    memset(&st7735_ctx, 0, sizeof(st7735_ctx));

    /* Конфигурация из примера WeAct 0.96" TFT */
    st7735_ctx.Orientation = ST7735_ORIENTATION_LANDSCAPE_ROT180;
    st7735_ctx.Panel       = HannStar_Panel;
    st7735_ctx.Type        = ST7735_0_9_inch_screen;

    if (ST7735_RegisterBusIO(&st7735, &st7735_io) != ST7735_OK)
    {
        printf("Display: ST7735_RegisterBusIO failed, continuing without display\r\n");
        return;
    }

    if (ST7735_Init(&st7735, ST7735_FORMAT_RBG565, &st7735_ctx) != ST7735_OK)
    {
        printf("Display: ST7735_Init failed, continuing without display\r\n");
        return;
    }
    display_ready = true;

    /* Единственная полная очистка экрана, дальше поля перекрывают друг друга сами */
    ST7735_FillRect(&st7735, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, C_BLACK);

    /* Первая отрисовка - синхронно, без ограничения по частоте */
    MarkDirty(F_ALL);
    while (Draw_NextField()) { }
    ui.last_draw_tick = HAL_GetTick();

    ST7735_DisplayOn(&st7735);
    HAL_GPIO_WritePin(LCD_BL_PORT, LCD_BL_PIN, LCD_BL_ON); // включаем подсветку
}

void Display_ShowBootProgress(const char *text, uint8_t percent)
{
    if (!display_ready) { return; }
    if (percent > 100U) { percent = 100U; }

    const int bar_w = DISPLAY_WIDTH - 8;

    Canvas_Begin(DISPLAY_WIDTH, CANVAS_MAX_H, C_BLACK);
    Canvas_Text(4, 0, (text != NULL) ? text : "", 16, 1, C_AMBER);
    Canvas_Rect(4, 20, bar_w, 10, C_IDLE);
    Canvas_Rect(5, 21, ((bar_w - 2) * (int)percent) / 100, 8, C_GREEN);
    Canvas_Flush(0, 24);
}

void Display_EndBootProgress(void)
{
    if (!display_ready) { return; }

    ST7735_FillRect(&st7735, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, C_BLACK);
    MarkDirty(F_ALL);   /* Display_Task() перерисует интерфейс */
}

void Display_Task(void)
{
    if (!display_ready) { return; }

    const uint32_t now = HAL_GetTick();

    /* Гасим временные ошибки */
    for (uint8_t i = 1; i < DISPLAY_ERR_COUNT; i++)
    {
        if (ui.errors[i].active && !error_info[i].sticky &&
            (int32_t)(now - ui.errors[i].expire) >= 0)
        {
            ui.errors[i].active = false;
            MarkDirty(F_HEADER);
        }
    }

    /* Энумерация затянулась: библиотека USB-хоста сама об этом не сообщает */
    if (ui.link == DISPLAY_LINK_ENUMERATING && !ui.errors[DISPLAY_ERR_USB_ENUM].active &&
        (now - ui.link_since) >= ENUM_TIMEOUT_MS)
    {
        Display_ShowError(DISPLAY_ERR_USB_ENUM);
    }

    if (ui.dirty == 0U || (now - ui.last_draw_tick) < MIN_DRAW_INTERVAL_MS)
    {
        return;
    }

    /* Не больше одного поля за вызов, чтобы не задерживать USB/MIDI */
    Draw_NextField();
    ui.last_draw_tick = HAL_GetTick();
}

/* ------------------------------------------------------------------------- */
/* Public API: connection                                                    */
/* ------------------------------------------------------------------------- */

void Display_SetLinkState(Display_Link_t state)
{
    if (ui.link == state) { return; }

    ui.link       = state;
    ui.link_since = HAL_GetTick();
    ui.errors[DISPLAY_ERR_USB_ENUM].active = false;   /* новое состояние линка - ошибка устарела */

    if (state != DISPLAY_LINK_READY)
    {
        /* Клавиатуры нет: сбрасываем всё, что от неё зависело */
        ui.sustain     = false;
        ui.voices_used = 0;

        if (ui.note_live)
        {
            ui.note_live = false;
            MarkDirty(F_NOTE);
        }
    }
    MarkDirty(F_HEADER);
}

void Display_SetMidiConnected(bool connected)
{
    if (connected)
    {
        Display_SetLinkState(DISPLAY_LINK_READY);
    }
    else if (ui.link == DISPLAY_LINK_READY)
    {
        /* Идущую энумерацию не затираем: ею управляет USB-колбэк */
        Display_SetLinkState(DISPLAY_LINK_NONE);
    }
}

void Display_SetDeviceName(const char *name)
{
    char tmp[sizeof(ui.device)];
    CopyText(tmp, sizeof(tmp), name);

    if (strcmp(tmp, ui.device) != 0)
    {
        memcpy(ui.device, tmp, sizeof(ui.device));
        MarkDirty(F_HEADER);
    }
}

/* ------------------------------------------------------------------------- */
/* Public API: notes                                                         */
/* ------------------------------------------------------------------------- */

void Display_SetLastNote(uint8_t note)
{
    Display_SetNoteState(NULL, 1, note);
}

void Display_SetNoteState(
    const uint8_t *active_notes,
    uint16_t active_note_count,
    uint8_t note_val)
{
    if (active_note_count == 0)
    {
        /* Клавиши отпущены: оставляем последнюю ноту/аккорд серым */
        if (ui.kind != NOTE_KIND_NONE && ui.note_live)
        {
            ui.note_live = false;
            MarkDirty(F_NOTE);
        }
        return;
    }

    NoteKind_t kind = NOTE_KIND_SINGLE;
    char chord[sizeof(ui.chord)] = "";

    if (active_note_count >= 3 && active_notes != NULL)
    {
        kind = NOTE_KIND_CHORD;

        /* Имя считаем сразу: active_notes действителен только на время вызова */
        if (!ChordDetector_Detect(active_notes, active_note_count, chord, sizeof(chord)))
        {
            snprintf(chord, sizeof(chord), "???");
        }
    }

    if (ui.kind == kind && ui.note_live && ui.note == note_val &&
        ui.key_count == active_note_count && strcmp(ui.chord, chord) == 0)
    {
        return;   /* ничего не изменилось */
    }

    ui.kind      = kind;
    ui.note_live = true;
    ui.note      = note_val;
    ui.key_count = active_note_count;
    memcpy(ui.chord, chord, sizeof(ui.chord));
    MarkDirty(F_NOTE);
}

void Display_SetVelocity(uint8_t velocity)
{
    if (ui.velocity == velocity) { return; }

    ui.velocity = velocity;
    if (ui.note_live && ui.kind == NOTE_KIND_SINGLE)
    {
        MarkDirty(F_NOTE);
    }
}

void Display_SetSustain(bool on)
{
    if (ui.sustain == on) { return; }

    ui.sustain = on;
    MarkDirty(F_HEADER);
}

/* ------------------------------------------------------------------------- */
/* Public API: instrument, voices, volume                                    */
/* ------------------------------------------------------------------------- */

void Display_SetPatch(uint8_t index, const char *name)
{
    char tmp[sizeof(ui.patch_name)];
    CopyText(tmp, sizeof(tmp), name);

    if (ui.patch_index == index && strcmp(tmp, ui.patch_name) == 0) { return; }

    ui.patch_index = index;
    memcpy(ui.patch_name, tmp, sizeof(ui.patch_name));
    MarkDirty(F_INSTR);
}

void Display_SetVoices(uint8_t used, uint8_t max)
{
    if (ui.voices_used == used && ui.voices_max == max) { return; }

    ui.voices_used = used;
    ui.voices_max  = max;
    MarkDirty(F_HEADER);
}

void Display_SetVolume(uint8_t percent)
{
    if (percent > 100U) { percent = 100U; }

    if (ui.volume_valid && ui.volume == percent) { return; }

    ui.volume_valid = true;
    ui.volume       = percent;
    MarkDirty(F_VOLUME);
}

void Display_SetClip(bool clipping)
{
    if (ui.clip == clipping) { return; }

    ui.clip = clipping;
    MarkDirty(F_VOLUME);
}

/* ------------------------------------------------------------------------- */
/* Public API: errors                                                        */
/* ------------------------------------------------------------------------- */

void Display_ShowError(Display_Error_t code)
{
    if (code <= DISPLAY_ERR_NONE || code >= DISPLAY_ERR_COUNT) { return; }

    ErrorSlot_t *slot = &ui.errors[code];

    slot->expire = HAL_GetTick() + ERROR_TIMEOUT_MS;

    if (!slot->active)
    {
        slot->active = true;
        slot->stamp  = ++ui.error_stamp;
        MarkDirty(F_HEADER);
    }
    /* Повторная ошибка только продлевает показ, без перерисовки */
}

void Display_ClearError(Display_Error_t code)
{
    if (code <= DISPLAY_ERR_NONE || code >= DISPLAY_ERR_COUNT) { return; }

    if (ui.errors[code].active)
    {
        ui.errors[code].active = false;
        MarkDirty(F_HEADER);
    }
}

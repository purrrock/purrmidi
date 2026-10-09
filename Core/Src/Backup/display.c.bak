#include "display.h"
#include "chord_detector.h"

#include "main.h"
#include "stm32h7xx_hal.h"

#include "st7735.h"
#include "font.h"

#include <stdio.h>
#include <string.h>

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

#define COLOR_BLACK      0x0000U
#define COLOR_WHITE      0xFFFFU

/* Максимальный размер глифа (шрифт 16 -> 8x16) */
#define GLYPH_MAX_W      8U
#define GLYPH_MAX_H      16U

extern SPI_HandleTypeDef hspi4;

static ST7735_Object_t st7735;
static ST7735_Ctx_t st7735_ctx;

static bool midi_connected = false;
static uint8_t last_note = 0;
static bool have_last_note = false;
static bool display_ready = false;   /* true только после успешной инициализации */
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
/* Text rendering                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Формат шрифта asc2_1206 / asc2_1608: глиф хранится по столбцам слева
 * направо, каждый столбец занимает (size + 7) / 8 байт, внутри байта
 * старший бит - верхняя строка.
 */
static void Display_DrawChar(
    uint16_t x,
    uint16_t y,
    char c,
    uint8_t size)
{
    if (c < ' ' || c > '~' || (size != 12U && size != 16U))
    {
        return;
    }

    const uint8_t width     = size / 2U;            /* 6 или 8 */
    const uint8_t col_bytes = (size + 7U) / 8U;     /* 2 для обоих шрифтов */
    const uint8_t *glyph    = (size == 12U) ? asc2_1206[c - ' ']
                                            : asc2_1608[c - ' '];

    uint16_t pixels[GLYPH_MAX_W * GLYPH_MAX_H];
    memset(pixels, 0, (size_t)width * size * sizeof(pixels[0]));

    for (uint8_t col = 0; col < width; col++)
    {
        for (uint8_t row = 0; row < size; row++)
        {
            uint8_t bits = glyph[col * col_bytes + (row >> 3)];

            if (bits & (0x80U >> (row & 7U)))
            {
                pixels[row * width + col] = COLOR_WHITE;
            }
        }
    }

    ST7735_FillRGBRect(&st7735, x, y, (uint8_t *)pixels, width, size);
}

/*
 * Каждый символ рисуется вместе с чёрным фоном, поэтому текст сам стирает
 * то, что было под ним. Если новая строка короче прежней, её нужно
 * дополнить пробелами - иначе останется "хвост" от старой.
 */
static void Display_DrawString(
    uint16_t x,
    uint16_t y,
    uint8_t size,
    const char *text)
{
    if (!display_ready) { return; }
	
    const uint16_t char_width = size / 2U;

    for (; *text != '\0' && x + char_width <= DISPLAY_WIDTH; text++)
    {
        Display_DrawChar(x, y, *text, size);
        x += char_width;
    }
}

/* ------------------------------------------------------------------------- */
/* Screen contents (перерисовываются по частям, без очистки всего экрана)    */
/* ------------------------------------------------------------------------- */

static void Display_DrawStatus(void)
{
    /* Обе строки по 18 символов, чтобы одна полностью затирала другую */
    Display_DrawString(
        2, 15, 12,
        midi_connected ? "MIDI: connected   " : "MIDI: disconnected");
}

static void Display_DrawNoteState(
    const uint8_t *active_notes,
    uint16_t active_note_count,
    uint8_t note_val)
{
    char text[20];

    if (active_note_count == 0)
    {
        /* Затираем область текстом из 18 пробелов (18 * 8px = 144px > DISPLAY_WIDTH) */
        Display_DrawString(2, 32, 16, "                  ");
        return;
    }

    if (active_note_count < 3)
    {
        /* Нота 0..11 -> октава -1, поэтому знаковый int, а не uint8_t */
        const int octave = (int)(note_val / 12U) - 1;

        snprintf(
            text,
            sizeof(text),
            "%s%d",
            note_names[note_val % 12U],
            octave);

        /* Дополняем пробелами до 18 символов для гарантированной очистки старого текста */
        size_t len = strlen(text);
        while (len < 18 && len < sizeof(text) - 1)
        {
            text[len++] = ' ';
        }
        text[len] = '\0';

        Display_DrawString(2, 32, 16, text);
    }
    else
    {
        char chord_name[16];
        if (ChordDetector_Detect(active_notes, active_note_count, chord_name, sizeof(chord_name)))
        {
            snprintf(text, sizeof(text), "ch %s", chord_name);
        }
        else
        {
            snprintf(text, sizeof(text), "ch ???");
        }

        size_t len = strlen(text);
        while (len < 18 && len < sizeof(text) - 1)
        {
            text[len++] = ' ';
        }
        text[len] = '\0';

        Display_DrawString(2, 32, 16, text);
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
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
	
    /* Единственная полная очистка экрана + статичный заголовок */
    ST7735_FillRect(&st7735, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, COLOR_BLACK);
    Display_DrawString(2, 1, 12, "PurrMidi");
    Display_DrawStatus();
    if (have_last_note)
    {
        Display_DrawNoteState(NULL, 1, last_note);
    }

    ST7735_DisplayOn(&st7735);
    HAL_GPIO_WritePin(LCD_BL_PORT, LCD_BL_PIN, LCD_BL_ON); // включаем подсветку
}

void Display_SetMidiConnected(bool connected)
{
    if (midi_connected == connected)
    {
        return;
    }

    midi_connected = connected;
    Display_DrawStatus();
}

void Display_SetLastNote(uint8_t note)
{
    last_note = note;
    have_last_note = true;
    Display_DrawNoteState(NULL, 1, note);
}

void Display_SetNoteState(
    const uint8_t *active_notes,
    uint16_t active_note_count,
    uint8_t note_val)
{
    last_note = note_val;
    have_last_note = (active_note_count > 0);
    Display_DrawNoteState(active_notes, active_note_count, note_val);
}

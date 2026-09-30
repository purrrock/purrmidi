#include "display.h"

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

#define LCD_SPI          SPI4

extern SPI_HandleTypeDef hspi4;

static ST7735_Object_t st7735;
static ST7735_Ctx_t st7735_ctx;

static bool midi_connected = false;
static uint8_t last_note = 0;
static bool have_last_note = false;

/* ------------------------------------------------------------------------- */
/* ST7735 bus interface                                                     */
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

static int32_t Display_ReadReg(
    uint8_t reg,
    uint8_t *data)
{
    int32_t result;

    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_RESET);

    result = HAL_SPI_Transmit(&hspi4, &reg, 1, 100);

    HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_SET);

    if (result == HAL_OK)
    {
        result = HAL_SPI_Receive(&hspi4, data, 1, 500);
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

static int32_t Display_ReceiveData(
    uint8_t *data,
    uint32_t length)
{
    int32_t result;

    HAL_GPIO_WritePin(LCD_CS_PORT, LCD_CS_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_SET);

    result = HAL_SPI_Receive(&hspi4, data, length, 500);

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

static void Display_DrawChar(
    uint16_t x,
    uint16_t y,
    char c,
    uint8_t size)
{
    if (c < ' ' || c > '~')
    {
        return;
    }

    if (size != 12U && size != 16U)
    {
        return;
    }

    uint8_t index = (uint8_t)(c - ' ');
    uint8_t width = (size == 12U) ? 6U : 8U;

uint16_t pixels[16][8];

    for (uint8_t row = 0; row < size; row++)
    {
        uint8_t bits;

        if (size == 12U)
        {
            bits = asc2_1206[index][row];
        }
        else
        {
            bits = asc2_1608[index][row];
        }

        for (uint8_t col = 0; col < width; col++)
        {
            bool pixel = (bits & (0x80U >> col)) != 0U;

            pixels[row][col] = pixel ? 0xFFFFU : 0x0000U;
        }
    }

    ST7735_FillRGBRect(
        &st7735,
        x,
        y,
        (uint8_t *)pixels,
        width,
        size);
}

static void Display_DrawString(
    uint16_t x,
    uint16_t y,
    uint8_t size,
    const char *text)
{
    uint16_t cursor = x;
    uint16_t char_width = size / 2U;

    while (*text != '\0')
    {
        if (cursor + char_width > DISPLAY_WIDTH)
        {
            break;
        }

        Display_DrawChar(
            cursor,
            y,
            *text,
            size);

        cursor += char_width;
        text++;
    }
}

static void Display_Redraw(void)
{
    char text[32];

    ST7735_FillRect(
        &st7735,
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        0x0000);

    /*
     * Line 1: small title
     */
    Display_DrawString(
        2,
        1,
        12,
        "PurrMidi");

    /*
     * Line 2: MIDI connection state
     */
    Display_DrawString(
        2,
        15,
        12,
        midi_connected ? "MIDI: connected" : "MIDI: disconnected");

    /*
     * Lines 3-4: last note
     */
    if (have_last_note)
    {
        uint8_t note = last_note % 12U;
        uint8_t octave = (last_note / 12U) - 1U;

        static const char *note_names[12] =
        {
            "C", "C#", "D", "D#", "E", "F",
            "F#", "G", "G#", "A", "A#", "B"
        };

        snprintf(
            text,
            sizeof(text),
            "%s%d",
            note_names[note],
            octave);

        Display_DrawString(
            2,
            32,
            16,
            text);
    }
    else
    {
        Display_DrawString(
            2,
            32,
            16,
            "---");
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void Display_Init(void)
{
       memset(&st7735_ctx, 0, sizeof(st7735_ctx));

    /*
     * This is the exact configuration used by the WeAct
     * 0.96" TFT example.
     */
    st7735_ctx.Orientation =
        ST7735_ORIENTATION_LANDSCAPE_ROT180;

    st7735_ctx.Panel = HannStar_Panel;
    st7735_ctx.Type = ST7735_0_9_inch_screen;

    if (ST7735_RegisterBusIO(
            &st7735,
            &st7735_io) != ST7735_OK)
    {
        Error_Handler();
    }

    if (ST7735_Init(
            &st7735,
            ST7735_FORMAT_RBG565,
            &st7735_ctx) != ST7735_OK)
    {
        Error_Handler();
    }

    ST7735_DisplayOn(&st7735);
    // Включаем подсветку явно
    HAL_GPIO_WritePin(LCD_BL_PORT, LCD_BL_PIN, GPIO_PIN_RESET);
    Display_Redraw();
}

void Display_SetMidiConnected(bool connected)
{
    if (midi_connected == connected)
    {
        return;
    }

    midi_connected = connected;
    Display_Redraw();
}

void Display_SetLastNote(uint8_t note)
{
    last_note = note;
    have_last_note = true;

    Display_Redraw();
}
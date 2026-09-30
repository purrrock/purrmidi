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

static SPI_HandleTypeDef hspi4;

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
    Display_BusInit,
    NULL,
    NULL,
    Display_WriteReg,
    Display_ReadReg,
    Display_SendData,
    Display_ReceiveData,
    Display_GetTick
};

/* ------------------------------------------------------------------------- */
/* SPI4                                                                      */
/* ------------------------------------------------------------------------- */

static void Display_SPI4_Init(void)
{
    hspi4.Instance = SPI4;

    hspi4.Init.Mode = SPI_MODE_MASTER;
    hspi4.Init.Direction = SPI_DIRECTION_1LINE;
    hspi4.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi4.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi4.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi4.Init.NSS = SPI_NSS_SOFT;

    hspi4.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;

    hspi4.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi4.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi4.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi4.Init.CRCPolynomial = 0;

    hspi4.Init.NSSPMode = SPI_NSS_PULSE_ENABLE;
    hspi4.Init.NSSPolarity = SPI_NSS_POLARITY_LOW;
    hspi4.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;

    hspi4.Init.TxCRCInitializationPattern =
        SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;

    hspi4.Init.RxCRCInitializationPattern =
        SPI_CRC_INITIALIZATION_ALL_ZERO_PATTERN;

    hspi4.Init.MasterSSIdleness =
        SPI_MASTER_SS_IDLENESS_00CYCLE;

    hspi4.Init.MasterInterDataIdleness =
        SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;

    hspi4.Init.MasterReceiverAutoSusp =
        SPI_MASTER_RX_AUTOSUSP_DISABLE;

    hspi4.Init.MasterKeepIOState =
        SPI_MASTER_KEEP_IO_STATE_DISABLE;

    hspi4.Init.IOSwap = SPI_IO_SWAP_DISABLE;

    if (HAL_SPI_Init(&hspi4) != HAL_OK)
    {
        Error_Handler();
    }
}

static void Display_SPI4_MspInit(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SPI4;
    PeriphClkInitStruct.Spi45ClockSelection =
        RCC_SPI45CLKSOURCE_D2PCLK1;

    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    __HAL_RCC_SPI4_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /*
     * WeAct:
     * PE12 -> SPI4_SCK
     * PE14 -> SPI4_MOSI
     */
    GPIO_InitStruct.Pin = GPIO_PIN_12 | GPIO_PIN_14;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI4;

    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);
}

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

    uint16_t pixels[8][16];

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
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /*
     * CS and D/C idle high.
     */
    HAL_GPIO_WritePin(
        LCD_CS_PORT,
        LCD_CS_PIN,
        GPIO_PIN_SET);

    HAL_GPIO_WritePin(
        LCD_DC_PORT,
        LCD_DC_PIN,
        GPIO_PIN_SET);

    /*
     * Backlight ON.
     * PE10 is the LCD backlight control on the WeAct board.
     */
    HAL_GPIO_WritePin(
        LCD_BL_PORT,
        LCD_BL_PIN,
        GPIO_PIN_SET);

    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin =
        LCD_CS_PIN |
        LCD_DC_PIN |
        LCD_BL_PIN;

    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    Display_SPI4_MspInit();
    Display_SPI4_Init();

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
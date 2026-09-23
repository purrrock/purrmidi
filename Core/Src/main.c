/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
#include "main.h"
#include "dma.h"
#include "sai.h"
#include "usart.h"
#include "usb_host.h"
#include "gpio.h"

/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "usbh_midi.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
static volatile uint32_t note_on_count = 0;
static volatile uint32_t note_off_count = 0;
static volatile uint32_t midi_usb_packets = 0;
static volatile uint32_t midi_events = 0;
static volatile uint32_t midi_queue_overruns = 0;

#define MIDI_EVENT_QUEUE_SIZE 256
typedef struct
{
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} MIDI_Event_t;

static volatile MIDI_Event_t midi_event_queue[MIDI_EVENT_QUEUE_SIZE];
static volatile uint8_t midi_queue_head = 0;
static volatile uint8_t midi_queue_tail = 0;
/* USER CODE END PV */

void SystemClock_Config(void);
static void MPU_Config(void);
void MX_USB_HOST_Process(void);

/* USER CODE BEGIN PFP */
static void MIDI_QueueEvent(uint8_t status, uint8_t data1, uint8_t data2);
static uint8_t MIDI_QueueGet(MIDI_Event_t *event);
static uint8_t MIDI_CIN_DataLength(uint8_t cin);
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */
int _write(int file, char *ptr, int len)
{
    HAL_UART_Transmit(&huart3, (uint8_t *)ptr, len, HAL_MAX_DELAY);
    return len;
}

extern ApplicationTypeDef Appli_state;
extern USBH_HandleTypeDef hUsbHostFS;

__ALIGN_BEGIN uint8_t midi_rx_buffer[64] __ALIGN_END;

/*
 * Return the number of valid MIDI bytes in one USB-MIDI event packet.
 * The first byte of the packet is the Cable Number/CIN header; bytes 1..3
 * contain MIDI data, of which only the number returned here is valid.
 */
static uint8_t MIDI_CIN_DataLength(uint8_t cin)
{
    switch (cin)
    {
        case 0x1: /* Miscellaneous function code: 1 byte */
        case 0x5: /* SysEx ends with 1 byte */
        case 0xF: /* Single-byte system common/message */
            return 1U;

        case 0x2: /* Two-byte system common message */
        case 0x6: /* SysEx ends with 2 bytes */
        case 0xC: /* Program Change */
        case 0xD: /* Channel Pressure */
            return 2U;

        case 0x3: /* Three-byte system common message */
        case 0x4: /* SysEx starts or continues */
        case 0x7: /* SysEx ends with 3 bytes */
        case 0x8: /* Note Off */
        case 0x9: /* Note On */
        case 0xA: /* Poly-KeyPress */
        case 0xB: /* Control Change */
        case 0xE: /* Pitch Bend */
            return 3U;

        case 0x0: /* Reserved / invalid */
        default:
            return 0U;
    }
}

void USBH_MIDI_ReceiveCallback(USBH_HandleTypeDef *phost)
{
    midi_usb_packets++;

    uint16_t length = USBH_MIDI_GetLastReceivedDataSize(phost);

    /* USB-MIDI data consists exclusively of 4-byte event packets. */
    length &= (uint16_t)~3U;

    for (uint16_t i = 0U; i < length; i += 4U)
    {
        const uint8_t *packet = &midi_rx_buffer[i];
        uint8_t cin = packet[0] & 0x0FU;
        uint8_t data_length = MIDI_CIN_DataLength(cin);

        if (data_length == 0U)
        {
            continue;
        }

        /*
         * Normalize 1- and 2-byte USB-MIDI messages to the queue's fixed
         * three-byte representation. Unused bytes are explicitly zeroed.
         */
        uint8_t status = packet[1];
        uint8_t data1 = (data_length >= 2U) ? packet[2] : 0U;
        uint8_t data2 = (data_length >= 3U) ? packet[3] : 0U;

        midi_events++;
        MIDI_QueueEvent(status, data1, data2);
    }

    /* Arm the next transfer only after this packet has been parsed. */
    USBH_MIDI_Receive(phost, midi_rx_buffer, sizeof(midi_rx_buffer));
}

static void MIDI_QueueEvent(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint8_t next = (uint8_t)((midi_queue_head + 1U) % MIDI_EVENT_QUEUE_SIZE);

    if (next == midi_queue_tail)
    {
        midi_queue_overruns++;
        return;
    }

    midi_event_queue[midi_queue_head].status = status;
    midi_event_queue[midi_queue_head].data1 = data1;
    midi_event_queue[midi_queue_head].data2 = data2;
    midi_queue_head = next;
}

static uint8_t MIDI_QueueGet(MIDI_Event_t *event)
{
    if (midi_queue_tail == midi_queue_head)
    {
        return 0U;
    }

    *event = midi_event_queue[midi_queue_tail];
    midi_queue_tail = (uint8_t)((midi_queue_tail + 1U) % MIDI_EVENT_QUEUE_SIZE);
    return 1U;
}
/* USER CODE END 0 */

int main(void)
{
    MPU_Config();
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_DMA_Init();
    MX_USART3_UART_Init();
    MX_SAI1_Init();
    MX_USB_HOST_Init();

    printf("Waiting for USB device to be attached...\r\n");

    ApplicationTypeDef previous_state = APPLICATION_IDLE;

    while (1)
    {
        MX_USB_HOST_Process();

        MIDI_Event_t event;
        while (MIDI_QueueGet(&event))
        {
            uint8_t command = event.status & 0xF0U;

            if (command == 0x90U && event.data2 != 0U)
            {
                note_on_count++;
            }
            else if (command == 0x80U ||
                     (command == 0x90U && event.data2 == 0U))
            {
                note_off_count++;
            }
        }

        if (Appli_state != previous_state)
        {
            if (Appli_state == APPLICATION_READY)
            {
                USBH_MIDI_Receive(&hUsbHostFS,
                                  midi_rx_buffer,
                                  sizeof(midi_rx_buffer));
            }

            previous_state = Appli_state;
        }

        static uint32_t last_report = 0U;
        if (HAL_GetTick() - last_report >= 10000U)
        {
            last_report = HAL_GetTick();
            printf("[STATS] USB pkts: %lu | MIDI evts: %lu | ON: %lu | OFF: %lu | Overruns: %lu\r\n",
                   midi_usb_packets, midi_events, note_on_count,
                   note_off_count, midi_queue_overruns);
        }
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 5;
    RCC_OscInitStruct.PLL.PLLN = 96;
    RCC_OscInitStruct.PLL.PLLP = 2;
    RCC_OscInitStruct.PLL.PLLQ = 10;
    RCC_OscInitStruct.PLL.PLLR = 2;
    RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
    RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    RCC_OscInitStruct.PLL.PLLFRACN = 0;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                                  RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
    RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
    {
        Error_Handler();
    }
}

void MPU_Config(void)
{
    MPU_Region_InitTypeDef MPU_InitStruct = {0};

    HAL_MPU_Disable();
    MPU_InitStruct.Enable = MPU_REGION_ENABLE;
    MPU_InitStruct.Number = MPU_REGION_NUMBER0;
    MPU_InitStruct.BaseAddress = 0x0;
    MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
    MPU_InitStruct.SubRegionDisable = 0x87;
    MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
    MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
    MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
    MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&MPU_InitStruct);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
}
#endif /* USE_FULL_ASSERT */

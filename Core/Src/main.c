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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "sai.h"
#include "usart.h"
#include "usb_host.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "usbh_midi.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

static volatile uint32_t note_on_count = 0;
static volatile uint32_t note_off_count = 0;
static volatile uint32_t midi_usb_packets = 0;
static volatile uint32_t midi_events = 0;
static volatile uint32_t midi_queue_overruns = 0;
static volatile uint32_t midi_receive_errors = 0;

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

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
void MX_USB_HOST_Process(void);

/* USER CODE BEGIN PFP */
static void MIDI_QueueEvent(uint8_t status, uint8_t data1, uint8_t data2);
static uint8_t MIDI_QueueGet(MIDI_Event_t *event);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// Перенаправление printf в UART
int _write(int file, char *ptr, int len) {
    if (HAL_UART_Transmit(&huart3, (uint8_t*)ptr, len, 10) == HAL_OK) {
        return len;
    }
    return 0;
}

extern ApplicationTypeDef Appli_state;
extern USBH_HandleTypeDef hUsbHostFS;

// объявление буфера с выравниванием по 32-битной границе:
__ALIGN_BEGIN uint8_t midi_rx_buffer[64] __ALIGN_END;

void USBH_MIDI_ReceiveCallback(USBH_HandleTypeDef *phost)
{
    midi_usb_packets++;
    uint16_t length = USBH_MIDI_GetLastReceivedDataSize(phost);

    for (uint16_t i = 0; i + 3 < length; i += 4)
    {
        uint8_t cin = midi_rx_buffer[i] & 0x0F;

        if (cin == 0x00)
        {
            continue;
        }

        uint8_t status = midi_rx_buffer[i + 1];
        uint8_t data1  = 0;
        uint8_t data2  = 0;

        switch (cin)
        {
            case 0x1:
            case 0x5:
            case 0xF:
                // 1 byte message (status only)
                break;

            case 0x2:
            case 0x6:
            case 0xC:
            case 0xD:
                // 2 byte message (status + data1)
                data1 = midi_rx_buffer[i + 2];
                break;

            case 0x3:
            case 0x4:
            case 0x7:
            case 0x8:
            case 0x9:
            case 0xA:
            case 0xB:
            case 0xE:
                // 3 byte message (status + data1 + data2)
                data1 = midi_rx_buffer[i + 2];
                data2 = midi_rx_buffer[i + 3];
                break;

            default:
                continue;
        }

        midi_events++;
        MIDI_QueueEvent(status, data1, data2);
    }

    if (USBH_MIDI_Receive(phost,
                          midi_rx_buffer,
                          sizeof(midi_rx_buffer)) != USBH_OK)
    {
        midi_receive_errors++;
    }
}

// функция помещения события в очередь
static void MIDI_QueueEvent(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint8_t next = (uint8_t)((midi_queue_head + 1) % MIDI_EVENT_QUEUE_SIZE);

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
// и функция извлечения:
static uint8_t MIDI_QueueGet(MIDI_Event_t *event)
{
    if (midi_queue_tail == midi_queue_head)
    {
        return 0;
    }

    *event = midi_event_queue[midi_queue_tail];

    midi_queue_tail =
        (uint8_t)((midi_queue_tail + 1) % MIDI_EVENT_QUEUE_SIZE);

    return 1;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART3_UART_Init();
  MX_SAI1_Init();
  MX_USB_HOST_Init();
  /* USER CODE BEGIN 2 */
  printf("Waiting for USB device to be attached...\r\n");
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  ApplicationTypeDef previous_state = APPLICATION_IDLE;
  while (1)
  {
    /* USER CODE END WHILE */
    MX_USB_HOST_Process();

    /* USER CODE BEGIN 3 */
MIDI_Event_t event;

while (MIDI_QueueGet(&event))
{
 //   printf("[MIDI] %02X %02X %02X\r\n", event.status, event.data1,  event.data2);
    uint8_t command = event.status & 0xF0;
    if (command == 0x90 && event.data2 != 0)
    {
        note_on_count++;
    }
    else if (command == 0x80 ||
             (command == 0x90 && event.data2 == 0))
    {
        note_off_count++;
    }
}

if (Appli_state != previous_state)
{
    if (Appli_state == APPLICATION_READY)
    {
        if (USBH_MIDI_Receive(&hUsbHostFS,
                              midi_rx_buffer,
                              sizeof(midi_rx_buffer)) != USBH_OK)
        {
            midi_receive_errors++;
        }
    }

    previous_state = Appli_state;
}

    static uint32_t last_report = 0;

    if (HAL_GetTick() - last_report >= 10000)
    {
        last_report = HAL_GetTick();

        printf("[STATS] USB pkts: %lu | MIDI evts: %lu | ON: %lu | OFF: %lu | Overruns: %lu | RX errors: %lu\r\n",
               midi_usb_packets,
               midi_events,
               note_on_count,
               note_off_count,
               midi_queue_overruns,
               midi_receive_errors);
    }

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
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

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
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

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
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
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

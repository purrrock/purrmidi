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
#include "usart.h"
#include "usb_host.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
// #include "usbh_hid.h"
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
static volatile uint32_t midi_events_processed = 0;
static volatile uint32_t midi_queue_overruns = 0;

#define MIDI_EVENT_QUEUE_SIZE 32
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
void MX_USB_HOST_Process(void);

/* USER CODE BEGIN PFP */
static void MIDI_QueueEvent(uint8_t status, uint8_t data1, uint8_t data2);
static uint8_t MIDI_QueueGet(MIDI_Event_t *event);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// Перенаправление printf в UART2
int _write(int file, char *ptr, int len) {
    HAL_UART_Transmit(&huart2, (uint8_t*)ptr, len, HAL_MAX_DELAY);
    return len;
}

extern ApplicationTypeDef Appli_state;
extern USBH_HandleTypeDef hUsbHostFS;

uint8_t midi_rx_buffer[64]; 

/* void USBH_MIDI_ReceiveCallback(USBH_HandleTypeDef *phost)
{
    uint16_t length = USBH_MIDI_GetLastReceivedDataSize(phost);
    for (uint16_t i = 0; i + 3 < length; i += 4)
    {
        uint8_t cin = midi_rx_buffer[i] & 0x0F;
        if (cin == 0x00)
        {
            continue;
        }
        uint8_t status   = midi_rx_buffer[i + 1];
        uint8_t data1    = midi_rx_buffer[i + 2];
        uint8_t data2    = midi_rx_buffer[i + 3];
        MIDI_QueueEvent(status, data1, data2);
    }
    USBH_MIDI_Receive(phost, midi_rx_buffer, sizeof(midi_rx_buffer));
}
*/

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
        uint8_t data1  = midi_rx_buffer[i + 2];
        uint8_t data2  = midi_rx_buffer[i + 3];

        midi_events++;

        MIDI_QueueEvent(status, data1, data2);
    }

    USBH_MIDI_Receive(phost,
                      midi_rx_buffer,
                      sizeof(midi_rx_buffer));
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

    midi_events_processed++;

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
  MX_USART2_UART_Init();
  MX_USB_HOST_Init();
  /* USER CODE BEGIN 2 */
  	printf("\r\n=======================================\r\n");
    printf("STM32 USB Host Mouse Init...\r\n");
    printf("USART is working perfectly!\r\n");
    printf("Waiting for USB device to be attached...\r\n");
    printf("=======================================\r\n");
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

if (MIDI_QueueGet(&event))
{
 //     midi_events_processed++;
  HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
 
  uint8_t command = event.status & 0xF0;

if (command == 0x90 && event.data2 != 0)
{
    note_on_count++;
    // printf("[MIDI] Note ON  | Note: %3d | Velocity: %3d\r\n", event.data1, event.data2);
}
else if (command == 0x80 || (command == 0x90 && event.data2 == 0))
{
    note_off_count++;
    // printf("[MIDI] Note OFF | Note: %3d\r\n", event.data1);
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
    static uint32_t last_report = 0;

    if (HAL_GetTick() - last_report >= 10000)
    {
        last_report = HAL_GetTick();

        printf("[MIDI] USB packets: %lu | events: %lu | processed: %lu | overruns: %lu\r\n",
               midi_usb_packets,
               midi_events,
               midi_events_processed,
               midi_queue_overruns);


        printf("[MIDI] Notes ON=%lu Notes OFF=%lu\r\n",
       note_on_count,
       note_off_count);
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

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 72;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 3;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

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

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
#include "fatfs.h"
#include "sai.h"
#include "sdmmc.h"
#include "spi.h"
#include "usart.h"
#include "usb_host.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "synth_engine.h" // синтезаторы epiano / pluck / sine; переключаются по MIDI Program Change
#include "display.h"
#include "midi_event.h"
#include "midi_queue.h"
#include "midi_usb.h"
#include "usb_port_recover.h"
#include "usbh_midi.h"
#include "midi_dispatch.h"
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

extern USBH_HandleTypeDef hUsbHostFS;

static volatile uint32_t note_on_count = 0;
static volatile uint32_t note_off_count = 0;

static uint8_t active_notes[128];
static uint8_t active_note_stack[128];
static uint16_t active_note_count = 0;

/* 1 = print [STATS] and [USBDIAG1..3] to UART (debug only), 0 = silent */
#define USB_DIAG_UART 0

/* Audio: 48 kHz, stereo, 16 bit. Circular DMA ring split into two halves.
 * It must NOT be placed in DTCM (DMA2 cannot reach it): own section in AXI SRAM,
 * see .dma_buffer in the linker script. D-Cache is off, so no cache maintenance needed. */
#define AUDIO_BUFFER_FRAMES  256U                      /* stereo frames in the whole ring (2 x 128) */
#define AUDIO_BUFFER_SIZE    (AUDIO_BUFFER_FRAMES * 2U) /* int16 items passed to SAI DMA */
static int16_t audio_buffer[AUDIO_BUFFER_SIZE] __attribute__((section(".dma_buffer"), aligned(32)));

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
void MX_USB_HOST_Process(void);

/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// Перенаправление printf в UART
int _write(int file, char *ptr, int len) {
    (void)file;
    uint32_t timeout = (uint32_t)((len * 10U) / 115U) + 20U;
    if (HAL_UART_Transmit(&huart3, (uint8_t*)ptr, len, timeout) == HAL_OK) {
        return len;
    }
    return 0;
}

#if USB_DIAG_UART
static void Print_USB_Diag(uint32_t max_loop_dt)
{
    MIDI_Diag_t diag = {0};
    if (USBH_MIDI_GetDiag(&hUsbHostFS, &diag) != USBH_OK)
    {
        memset(&diag, 0, sizeof(diag));
    }

    uint32_t last_tick = MIDI_USB_GetLastPacketTick();
    uint32_t now = HAL_GetTick();
    uint32_t silence_ms = now - last_tick;

    printf("[USBDIAG1] dt_max: %lu ms | rearm: %lu | silence: %lu ms | transfers: %lu NOTREADY: %lu ERR: %lu STALL: %lu STALL_IDLE: %lu\r\n",
           max_loop_dt,
           MIDI_USB_GetRearmCount(),
           silence_ms,
           diag.urb_done_cnt,
           diag.urb_notready_cnt,
           diag.urb_error_cnt,
           diag.urb_stall_cnt,
           diag.stall_to_idle_cnt);

    printf("[USBDIAG2] RX bytes: %lu events4: %lu | state: %u rx_state: %u | Ep: 0x%02X type: %u size: %u interval: %u\r\n",
           diag.rx_bytes_cnt,
           diag.usb_midi_packet_cnt,
           diag.state,
           diag.data_rx_state,
           diag.in_ep,
           diag.in_ep_type,
           diag.in_ep_size,
           diag.in_ep_interval);

    printf("[USBDIAG3] HCD ch: %u | HC state: %u urb: %u xfer: %lu err: %u toggle_in: %u\r\n",
           diag.hcd_channel,
           diag.hc_state,
           diag.hc_urb_state,
           (unsigned long)diag.hc_xfer_count,
           diag.hc_err_cnt,
           diag.hc_toggle_in);
}
#endif /* USB_DIAG_UART */

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
  MX_SPI4_Init();
  MX_SDMMC1_SD_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */
  Display_Init();
  MIDI_Queue_Init();
  MIDI_USB_Init();
  SynthEngine_Init(); // Включает синтезатор по умолчанию (E-Piano); дальше выбор — по Program Change
  printf("Synth engine: %s\r\n", SynthEngine_GetName());
  // Запуск круговой передачи DMA на ЦАП PCM5102A для SAI1_A
  memset(audio_buffer, 0, sizeof(audio_buffer));  /* .dma_buffer is not zeroed by startup */
  if (HAL_SAI_Transmit_DMA(&hsai_BlockA1, (uint8_t *)audio_buffer, AUDIO_BUFFER_SIZE) != HAL_OK)
  {
    Error_Handler();
  }
  printf("Waiting for USB device to be attached...\r\n");
  HAL_GPIO_WritePin(DEBUG_LED_GPIO_Port, DEBUG_LED_Pin, GPIO_PIN_RESET);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

#if USB_DIAG_UART
  uint32_t prev_loop_tick = HAL_GetTick();
  uint32_t max_loop_dt = 0;
  static bool silent_episode_reported = false;
  static uint32_t last_report = 0;
#endif

  while (1)
  {
#if USB_DIAG_UART
    uint32_t current_tick = HAL_GetTick();
    uint32_t loop_dt = current_tick - prev_loop_tick;
    prev_loop_tick = current_tick;
    if (loop_dt > max_loop_dt)
    {
        max_loop_dt = loop_dt;
    }
#endif

    /* USER CODE END WHILE */
    MX_USB_HOST_Process();

    /* USER CODE BEGIN 3 */
    USB_PortLostRecover(&hUsbHostFS);   /* port silently disabled by HW -> re-enumerate */
    MIDI_USB_Process();

    if (MIDI_USB_HasStateChanged())
    {
        Display_SetMidiConnected(MIDI_USB_IsConnected());
    }

    MIDI_Event_t event;

    while (MIDI_Queue_Pop(&event))
    {
        // printf("[MIDI] %02X %02X %02X\r\n", event.status, event.data1, event.data2);
        MIDI_Dispatch(&event);   /* Note On/Off, CC -> активный синтезатор (lock-free FIFO в аудио-ISR); Program Change -> смена синтезатора */

        uint8_t command = event.status & MIDI_STATUS_MASK;
        uint8_t note = event.data1;

        if (command == MIDI_STATUS_PROGRAM_CHANGE)
        {
            printf("[PC] program %u -> %s\r\n", event.data1, SynthEngine_GetName());
        }

        if (command == MIDI_STATUS_NOTE_ON && event.data2 != 0)
        {
            note_on_count++;

            if (note < 128 && active_notes[note] == 0)
            {
                active_notes[note] = 1;
                active_note_stack[active_note_count] = note;
                active_note_count++;
            }

            HAL_GPIO_WritePin(
                DEBUG_LED_GPIO_Port,
                DEBUG_LED_Pin,
                GPIO_PIN_SET
            );

            uint8_t last_active_note = (active_note_count > 0) ? active_note_stack[active_note_count - 1] : 0;
            Display_SetNoteState(active_notes, active_note_count, last_active_note);
        }
        else if (command == MIDI_STATUS_NOTE_OFF ||
                 (command == MIDI_STATUS_NOTE_ON && event.data2 == 0))
        {
            note_off_count++;

            if (note < 128 && active_notes[note] != 0)
            {
                active_notes[note] = 0;

                /* Удаляем ноту из active_note_stack */
                for (uint16_t i = 0; i < active_note_count; i++)
                {
                    if (active_note_stack[i] == note)
                    {
                        for (uint16_t j = i; j < active_note_count - 1; j++)
                        {
                            active_note_stack[j] = active_note_stack[j + 1];
                        }
                        break;
                    }
                }

                if (active_note_count > 0)
                {
                    active_note_count--;
                }
            }

            uint8_t last_active_note = (active_note_count > 0) ? active_note_stack[active_note_count - 1] : 0;
            Display_SetNoteState(active_notes, active_note_count, last_active_note);

            if (active_note_count == 0)
            {
                HAL_GPIO_WritePin(
                    DEBUG_LED_GPIO_Port,
                    DEBUG_LED_Pin,
                    GPIO_PIN_RESET
                );
            }
        }
    }

#if USB_DIAG_UART
    uint32_t now = HAL_GetTick();
    uint32_t silence_ms = now - MIDI_USB_GetLastPacketTick();
    if (!MIDI_USB_IsConnected() || silence_ms <= 3000)
    {
        silent_episode_reported = false;
    }
    else if (MIDI_USB_IsConnected() && silence_ms > 3000 && !silent_episode_reported)
    {
        Print_USB_Diag(max_loop_dt);
        silent_episode_reported = true;
        prev_loop_tick = HAL_GetTick();
    }
    if (now - last_report >= 10000)
    {
        last_report = now;
        printf("[STATS] USB pkts: %lu | MIDI evts: %lu | ON: %lu | OFF: %lu | Overruns: %lu | RX errors: %lu\r\n",
               MIDI_USB_GetPacketsCount(),
               MIDI_USB_GetEventsCount(),
               note_on_count,
               note_off_count,
               MIDI_Queue_GetOverrunCount(),
               MIDI_USB_GetReceiveErrorsCount());
        Print_USB_Diag(max_loop_dt);
        max_loop_dt = 0;
        prev_loop_tick = HAL_GetTick();
    }
#endif
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
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 5;
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
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* Audio DMA callbacks (DMA2_Stream0 ISR): refill the half that was just played.
 * Half-transfer -> first half is free; transfer-complete -> second half is free. */
void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai->Instance == SAI1_Block_A) {
        SynthEngine_FillStereoBuffer(&audio_buffer[0], AUDIO_BUFFER_FRAMES / 2);
    }
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai->Instance == SAI1_Block_A) {
        SynthEngine_FillStereoBuffer(&audio_buffer[AUDIO_BUFFER_SIZE / 2], AUDIO_BUFFER_FRAMES / 2);
    }
}

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

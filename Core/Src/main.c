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
#include "quadspi.h"
#include "sai.h"
#include "spi.h"
#include "usart.h"
#include "usb_host.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "synth_engine.h" /* Синтезаторы epiano / pluck / lead / organ / violin / soundfont */
#include "violin_synth.h"
#include "display.h"
#include "midi_event.h"
#include "midi_queue.h"
#include "midi_usb.h"
#include "usb_port_recover.h"
#include "usbh_midi.h"
#include "midi_dispatch.h"
#include "soundfont_synth.h"
#include "diag.h"
#include "organ_synth.h"
#include "xip_mpu.h"   /* MPU-регионы и D-cache для XIP-чтения SoundFont из QSPI Flash */
#include "sf2_loader.h"
#include "audio_halves.h"
#include "watchdog.h"
#include "backup.h"

/* Статистика кучи malloc (Core/Src/sysmem.c) */
void Sysmem_GetHeapStats(uint32_t *used_bytes, uint32_t *total_bytes);
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


/* Ошибки SAI/DMA: растёт в ISR, главный цикл показывает E02 на дисплее */
static volatile uint32_t sai_error_count = 0;

static uint8_t active_notes[128];
static uint8_t active_note_stack[128];
static uint16_t active_note_count = 0;

/* Some keyboards send the "next instrument" Program Change 127 twice per button press
 * (e.g. on press and on release). Repeats closer than this are ignored. */
#define PC_NEXT_REPEAT_GUARD_MS 300U

/* 1 = enable the Cortex-M7 instruction cache. The code runs from flash, which is slow at 480 MHz
 * without a cache (several wait states per fetch). */
#define ENABLE_ICACHE 1

/* 1 = periodically print render-time statistics on the UART:
 *   [AUDIO] soundfont: fill max=NNN us (NN% of 2666 us budget) ...
 * Default: ON in Debug builds only (CubeMX defines DEBUG for the Debug configuration), OFF in Release, so the log
 * stays quiet on the device. Force with -DAUDIO_DIAG_UART=1. The timing itself is always measured, and a real overrun
 * (render longer than its budget) is still reported once via "[AUDIO] RENDER OVERRUN" below.
 * A render that takes longer than its budget (one DMA half-buffer) starves the main loop
 * (USB, MIDI, display): the render runs in PendSV, which preempts the main loop. */
#ifndef AUDIO_DIAG_UART
#ifdef DEBUG
#define AUDIO_DIAG_UART 1
#else
#define AUDIO_DIAG_UART 0
#endif
#endif

static volatile uint32_t audio_cyc_max = 0;      /* longest fill so far (CPU cycles) */
static volatile uint32_t audio_cyc_last = 0;
static volatile uint32_t audio_calls = 0;
static volatile uint32_t audio_over80 = 0;       /* fills that used > 80% of the budget */
static volatile uint32_t audio_over100 = 0;      /* fills that used > 100% of the budget */
static volatile bool     audio_reset_stats = false;
static volatile uint32_t audio_shed_blocks = 0;  /* blocks still to be output as silence (CPU load shedding) */
static volatile uint32_t audio_shed_total = 0;   /* blocks dropped so far (load shedding + blocks that missed their deadline) */

/* Which halves of the DMA ring wait for new samples. The DMA interrupt only marks a half as free and pends PendSV;
 * the synth is rendered in PendSV (lowest priority), so USB/SDMMC/SysTick can preempt it. See audio_halves.h. */
static AudioHalves audio_halves;
static volatile uint32_t audio_blocks_done = 0;   /* halves processed by PendSV so far: proof that the audio is alive */

/* Watchdog policy (see watchdog.h): the main loop kicks the IWDG at most every WATCHDOG_CHECK_MS and only if the audio
 * made progress since the previous check. A hung main loop AND a stopped audio (lost DMA/PendSV) both end in a reset. */
#define WATCHDOG_CHECK_MS  100U
/* Audio: 48 kHz, stereo, 16 bit. Circular DMA ring split into two halves.
 * It must NOT be placed in DTCM (DMA2 cannot reach it): it lives in .dma_buffer, i.e. D2 SRAM (see the linker script).
 * The D-cache is ON, but XipMpu_Config() marks D2 SRAM non-cacheable (AXI SRAM is write-back cacheable and holds no
 * DMA buffers), so no cache maintenance is needed for DMA. Any new DMA buffer must also go into .dma_buffer. */
#define AUDIO_BUFFER_FRAMES  256U                      /* stereo frames in the whole ring (2 x 128) */
#define AUDIO_BUFFER_SIZE    (AUDIO_BUFFER_FRAMES * 2U) /* int16 items passed to SAI DMA */
static int16_t audio_buffer[AUDIO_BUFFER_SIZE] __attribute__((section(".dma_buffer"), aligned(32)));

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
void MX_USB_HOST_Process(void);

/* USER CODE BEGIN PFP */
static void watchdog_service(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Перенаправление вывода printf в UART3 (через Diag_Write: согласовано с выводом из прерываний) */
int _write(int file, char *ptr, int len) {
    (void)file;
    return Diag_Write(ptr, len);
}

/* Обработка кнопки K1 (PC13) с неблокирующим программным дребезгом (50 ms) */
static void Button_K1_Process(void)
{
    static bool button_debounced_state = false;
    static bool button_last_raw = false;
    static uint32_t button_change_tick = 0;

    bool raw_state = (HAL_GPIO_ReadPin(USER_KEY_K1_GPIO_Port, USER_KEY_K1_Pin) == GPIO_PIN_SET);
    uint32_t now = HAL_GetTick();

    if (raw_state != button_last_raw)
    {
        button_last_raw = raw_state;
        button_change_tick = now;
    }

    if ((now - button_change_tick) >= 50U)
    {
        if (button_debounced_state != button_last_raw)
        {
            button_debounced_state = button_last_raw;
            if (button_debounced_state)
            {
                /* Нажатие K1: отправка Program Change 127 */
                MIDI_Event_t evt = {
                    .status = MIDI_STATUS_PROGRAM_CHANGE,
                    .data1 = 127,
                    .data2 = 0
                };
                MIDI_Queue_Push(&evt);
            }
        }
    }
}

/* Номер на дисплее 1-based (как на большинстве клавиатур): Program Change 0 -> "01".
 * Имя берётся у диспетчера: для пресетов загруженного SF2 это "SF:<имя пресета>". */
static void Display_SyncPatch(void)
{
    char name[18];   /* до 17 символов + '\0' (ширина поля дисплея) */
    const uint32_t program = SynthEngine_GetProgram();
    SynthEngine_GetProgramName(program, name, sizeof(name));
    Display_SetPatch((uint8_t)((program + 1U) > 255U ? 255U : (program + 1U)), name);
}

/* Уровень CC 0..127 -> проценты для полосы громкости */
static void Display_SyncVolume(void)
{
    Display_SetVolume((uint8_t)(((uint32_t)SynthEngine_GetMasterVolume() * 100U + 63U) / 127U));
}

/* ---- Сохранение выбора (программа Program Change + громкость) в резервных регистрах RTC (см. backup.h) ----
 * Работает и без батарейки на VBAT: тогда после выключения питания блок пуст, и прибор стартует с настройками
 * по умолчанию (E-Piano, громкость SYNTH_MASTER_VOLUME_DEFAULT); об этом сообщается в UART. */
#define SETTINGS_SAVE_DELAY_MS 1000U   /* запись не раньше чем через 1 с после последнего изменения (ручки шлют CC7 потоком) */

static bool     settings_dirty = false;
static uint32_t settings_dirty_tick = 0U;

static void Settings_MarkDirty(void)
{
    settings_dirty = true;
    settings_dirty_tick = HAL_GetTick();
}

static void Settings_Service(void)
{
    if (settings_dirty && (HAL_GetTick() - settings_dirty_tick) >= SETTINGS_SAVE_DELAY_MS)
    {
        settings_dirty = false;
        Backup_SaveSettings((uint16_t)SynthEngine_GetProgram(), SynthEngine_GetMasterVolume());
    }
}

/* Вызывать после SoundFontSynth_InitSF2(): допустимый номер программы зависит от числа пресетов загруженного SF2. */
static void Settings_Restore(void)
{
    uint16_t program = 0U;
    uint8_t  volume  = SYNTH_MASTER_VOLUME_DEFAULT;

    if (!Backup_LoadSettings(&program, &volume))
    {
        printf("[SET] no saved settings - defaults: %s, volume %u\r\n",
               SynthEngine_GetName(), (unsigned)SynthEngine_GetMasterVolume());
        return;
    }

    if (!SynthEngine_SelectProgram(program) || !SynthEngine_IsAvailable(SynthEngine_GetCurrent()))
    {
        printf("[SET] saved program %u is not available (SoundFont bank changed?) - default engine\r\n",
               (unsigned)program);
        SynthEngine_Select(SYNTH_ENGINE_DEFAULT);
    }
    SynthEngine_SetMasterVolume(volume);
    Display_SyncPatch();
    Display_SyncVolume();
    printf("[SET] restored: program %u (%s), volume %u\r\n",
           (unsigned)SynthEngine_GetProgram(), SynthEngine_GetName(), (unsigned)SynthEngine_GetMasterVolume());
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
  Diag_EnableCycleCounter();
#if ENABLE_ICACHE
  SCB_EnableICache();
#endif
  /* D-cache нужен для быстрого XIP-чтения сэмплов из QSPI Flash и данных движков в AXI SRAM (write-back). DMA-буферы
   * лежат в D2 SRAM (.dma_buffer), она non-cacheable (MPU, см. xip_mpu.c). */
  XipMpu_Config();
  SCB_EnableDCache();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  Watchdog_Start();     /* IWDG1 с загрузочным таймаутом ~8 с; до main-цикла «кормится» вручную */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART3_UART_Init();
  MX_SAI1_Init();
  MX_USB_HOST_Init();
  MX_SPI4_Init();
  MX_QUADSPI_Init();
  /* USER CODE BEGIN 2 */
  Backup_Init();                                  /* резервные регистры RTC (VBAT); без батарейки - пустой блок */
  Backup_NoteReset(Watchdog_GetResetFlags());
  Watchdog_ReportResetCause();
  Backup_ReportPrevious();                        /* счётчики и запись о предыдущем сбое */
  Display_Init();
  Watchdog_Kick();
  MIDI_Queue_Init();
  MIDI_USB_Init();
  SynthEngine_Init(); // Включает синтезатор по умолчанию (E-Piano); дальше выбор — по Program Change
  printf("Synth engine: %s\r\n", SynthEngine_GetName());
  Display_SyncPatch();
  Display_SyncVolume();
  Watchdog_Kick();
#ifdef __OPTIMIZE__
  printf("[SYS] build: optimized (main.c compiled with -O1..-O3/-Os)\r\n");
#else
  printf("[SYS] build: NOT OPTIMIZED (-O0, Debug) - code runs several times slower than in Release; "
         "benchmarks and audio load below are pessimistic\r\n");
#endif
  printf("[SYS] SYSCLK=%lu Hz, I-cache %s, audio budget %lu us per %u frames\r\n",
         (unsigned long)SystemCoreClock, ENABLE_ICACHE ? "ON" : "off",
         (unsigned long)(1000000UL * (AUDIO_BUFFER_FRAMES / 2U) / 48000UL), (unsigned)(AUDIO_BUFFER_FRAMES / 2U));
  /* SoundFont-банк хранится только в QSPI Flash и читается через XIP. SD-карта нужна лишь здесь, при запуске:
   * если на ней лежит новый/изменённый 0:/SNDFNT.SF2, он сначала переписывается во флеш (до ~минуты, прогресс на
   * дисплее и в UART), после чего SD отключается. См. docs/SOUNDFONT.md. */
  printf("[SF2] Preparing SoundFont (QSPI XIP, SD is used only to update the bank)...\r\n");
  if (SoundFontSynth_InitSF2())
  {
    printf("[SF2] SoundFont ready\r\n");
  }
  else
  {
    printf("[SF2] SoundFont NOT available - the soundfont engine will be skipped\r\n");
  }
  Watchdog_Kick();
  Settings_Restore();     /* после загрузки SF2: от числа его пресетов зависит допустимый номер программы */
  {
    /* Куча занята в основном метаданными SoundFont. Сообщаем только если запас мал (<20%):
     * нормальная работа UART не засоряет. */
    uint32_t heap_used = 0U, heap_total = 0U;
    Sysmem_GetHeapStats(&heap_used, &heap_total);
    if (heap_total != 0U && heap_used > heap_total - heap_total / 5U)
    {
      printf("[MEM] WARNING: heap %lu of %lu KB used (SoundFont metadata); raise _Axi_Heap_Size\r\n",
             (unsigned long)(heap_used / 1024U), (unsigned long)(heap_total / 1024U));
    }
  }
  // Запуск круговой передачи DMA на ЦАП PCM5102A для SAI1_A
  memset(audio_buffer, 0, sizeof(audio_buffer));  /* silence from the very first DMA cycle (also after a soft reset) */
  AudioHalves_Init(&audio_halves);
  if (HAL_SAI_Transmit_DMA(&hsai_BlockA1, (uint8_t *)audio_buffer, AUDIO_BUFFER_SIZE) != HAL_OK)
  {
    Error_Handler();
  }
  printf("Waiting for USB device to be attached...\r\n");
  HAL_GPIO_WritePin(DEBUG_LED_GPIO_Port, DEBUG_LED_Pin, GPIO_PIN_RESET);
  Watchdog_EnterRuntime();    /* инициализация закончена: рабочий таймаут ~1,5 с */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  uint32_t seen_queue_overruns = MIDI_Queue_GetOverrunCount();
  uint32_t seen_rx_errors      = MIDI_USB_GetReceiveErrorsCount();
  uint32_t seen_sai_errors     = sai_error_count;
  uint32_t seen_audio_shed     = audio_shed_total;
  bool     sf2_missing_shown   = false;

  while (1)
  {
    /* USER CODE END WHILE */
    MX_USB_HOST_Process();

    /* USER CODE BEGIN 3 */
    /* ВАЖНО: весь пользовательский код цикла - только внутри USER CODE, иначе CubeMX удалит его при
     * перегенерации (так пропадал watchdog_service() и сторожевой таймер перезапускал прошивку). */
    watchdog_service();
    Diag_Flush();                       /* сообщения, отложенные из прерываний (см. diag.h) */
    Settings_Service();                 /* отложенная запись выбора/громкости в резервные регистры RTC */
    Button_K1_Process();
    USB_PortLostRecover(&hUsbHostFS);   /* port silently disabled by HW -> re-enumerate */
    MIDI_USB_Process();

    if (MIDI_USB_HasStateChanged())
    {
        const bool usb_connected = MIDI_USB_IsConnected();
        static bool usb_was_connected = false;

        Display_SetMidiConnected(usb_connected);

        /* Клавиатуру выдернули при зажатых клавишах: Note Off уже не придут -> без этого ноты
         * «залипают» (звук, зависшая индикация и светодиод). Гасим звук и сбрасываем список клавиш. */
        if (usb_was_connected && !usb_connected)
        {
            SynthEngine_Panic();
            memset(active_notes, 0, sizeof(active_notes));
            active_note_count = 0;
            Display_SetNoteState(active_notes, 0, 0);
            Display_SetSustain(false);
            HAL_GPIO_WritePin(DEBUG_LED_GPIO_Port, DEBUG_LED_Pin, GPIO_PIN_RESET);
        }
        usb_was_connected = usb_connected;
    }

    MIDI_Event_t event;

    while (MIDI_Queue_Pop(&event))
    {
        // printf("[MIDI] %02X %02X %02X\r\n", event.status, event.data1, event.data2);

        if ((event.status & MIDI_STATUS_MASK) == MIDI_STATUS_PROGRAM_CHANGE && event.data1 == 127)
        {
            static bool pc_next_seen = false;
            static uint32_t pc_next_tick = 0;
            uint32_t pc_now = HAL_GetTick();

            if (pc_next_seen && (pc_now - pc_next_tick) < PC_NEXT_REPEAT_GUARD_MS)
            {
                continue;   /* повтор игнорируем молча */
            }
            pc_next_seen = true;
            pc_next_tick = pc_now;
        }

        MIDI_Dispatch(&event);   /* Note On/Off, CC -> активный синтезатор (lock-free FIFO в аудио-ISR); Program Change -> смена синтезатора */

        uint8_t command = event.status & MIDI_STATUS_MASK;
        uint8_t note = event.data1;

        if (command == MIDI_STATUS_PROGRAM_CHANGE)
        {
            printf("[PC] ch=%u program %u -> %s\r\n", event.status & 0x0FU, event.data1, SynthEngine_GetName());
            audio_reset_stats = true;   /* separate timing statistics for every engine */
            Display_SyncPatch();
            Settings_MarkDirty();
        }
        else if (command == MIDI_STATUS_CONTROL_CHANGE)
        {
            if (event.data1 == SYNTH_MASTER_VOLUME_CC)
            {
                Display_SyncVolume();
                Settings_MarkDirty();
            }
            else if (event.data1 == 64U)
            {
                Display_SetSustain(event.data2 >= 64U);
            }
        }

        if (command == MIDI_STATUS_NOTE_ON && event.data2 != 0)
        {
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
            Display_SetVelocity(event.data2);
            uint8_t last_active_note = (active_note_count > 0) ? active_note_stack[active_note_count - 1] : 0;
            Display_SetNoteState(active_notes, active_note_count, last_active_note);
        }
        else if (command == MIDI_STATUS_NOTE_OFF ||
                 (command == MIDI_STATUS_NOTE_ON && event.data2 == 0))
        {
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

#if AUDIO_DIAG_UART
    {
        /* Report only when a new maximum is reached (or the budget was exceeded), so the log stays quiet. */
        static uint32_t last_audio_report = 0;
        static uint32_t reported_max = 0;
        static uint32_t reported_over80 = 0;
        static uint32_t reported_shed = 0;
        static uint32_t reported_late = 0;
        uint32_t t_now = HAL_GetTick();

        if (audio_reset_stats)
        {
            audio_reset_stats = false;
            SoundFontSynth_ResetStats();
            audio_cyc_max = 0;
            audio_over80 = 0;
            audio_over100 = 0;
            reported_max = 0;
            reported_over80 = 0;
        }
        if (t_now - last_audio_report >= 250U)
        {
            last_audio_report = t_now;
            uint32_t m = audio_cyc_max;
            uint32_t o80 = audio_over80;
            if (m > reported_max + reported_max / 8U || o80 != reported_over80 || audio_shed_total != reported_shed ||
                AudioHalves_LateCount(&audio_halves) != reported_late)
            {
                const uint32_t us = SystemCoreClock / 1000000U;
                const uint32_t budget = (SystemCoreClock / 48000U) * (AUDIO_BUFFER_FRAMES / 2U);
                printf("[AUDIO] %s: fill last=%lu us max=%lu us (%lu%% of %lu us) >80%%: %lu >100%%: %lu shed=%lu late=%lu calls=%lu\r\n",
                       SynthEngine_GetName(),
                       (unsigned long)(audio_cyc_last / us), (unsigned long)(m / us),
                       (unsigned long)(m / (budget / 100U)), (unsigned long)(budget / us),
                       (unsigned long)o80, (unsigned long)audio_over100, (unsigned long)audio_shed_total,
                       (unsigned long)AudioHalves_LateCount(&audio_halves), (unsigned long)audio_calls);
                reported_max = m;
                reported_over80 = o80;
                reported_shed = audio_shed_total;
                reported_late = AudioHalves_LateCount(&audio_halves);
            }
        }

        /* SoundFont statistics: render time, voices, clipping (one line per second while notes are playing). */
        if (SynthEngine_GetCurrent() == SYNTH_ENGINE_SOUNDFONT)
        {
            SoundFontSynth_ReportStats(t_now);
        }
    }
#endif

    /* Новые ошибки -> красная плашка на дисплее (гаснет сама через 3 с) */
    {
        const uint32_t queue_overruns = MIDI_Queue_GetOverrunCount();
        if (queue_overruns != seen_queue_overruns)
        {
            seen_queue_overruns = queue_overruns;
            Display_ShowError(DISPLAY_ERR_MIDI_QUEUE);
        }

        const uint32_t rx_errors = MIDI_USB_GetReceiveErrorsCount();
        if (rx_errors != seen_rx_errors)
        {
            seen_rx_errors = rx_errors;
            Display_ShowError(DISPLAY_ERR_USB_RX);
        }

        const uint32_t sai_errors = sai_error_count;
        if (sai_errors != seen_sai_errors)
        {
            seen_sai_errors = sai_errors;
            Display_ShowError(DISPLAY_ERR_AUDIO_DMA);
        }

        /* Load shedding и пропущенные сроки: блоки заменены тишиной или проиграны недозаполненными,
         * т.е. звук реально пострадал (счётчики не сбрасываются) */
        const uint32_t shed = audio_shed_total + AudioHalves_LateCount(&audio_halves);
        if (shed != seen_audio_shed)
        {
            seen_audio_shed = shed;
            Display_ShowError(DISPLAY_ERR_AUDIO_OVERLOAD);
        }

        /* SoundFont выбран, но загрузка не удалась (нет SD / файла / памяти): один раз на выбор */
        const bool sf2_missing = (SynthEngine_GetCurrent() == SYNTH_ENGINE_SOUNDFONT) &&
                                 !SynthEngine_IsAvailable(SYNTH_ENGINE_SOUNDFONT);
        if (sf2_missing && !sf2_missing_shown)
        {
            Display_ShowError(DISPLAY_ERR_SF2);
        }
        sf2_missing_shown = sf2_missing;
    }

    Display_Task();   /* рисует не более одного "грязного" поля за вызов */
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

/* Audio rendering architecture
 * ----------------------------
 * DMA2_Stream0 ISR (priority 0, a few microseconds): HAL calls the callbacks below, which only record which half of
 * the ring was just played (AudioHalves_Request) and set the PendSV pending bit.
 * PendSV (priority 15 = lowest, preempts only the main loop): Audio_ProcessPending() renders the synth into the free
 * half. Every other interrupt - USB (0), SAI errors (1), SDMMC (5), SysTick (14) - preempts the render, so a block
 * that takes up to 2.67 ms no longer blocks USB and the HAL tick.
 * Half-transfer -> first half is free; transfer-complete -> second half is free. */
static inline void audio_request_half(unsigned half)
{
    (void)AudioHalves_Request(&audio_halves, half);
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;      /* run Audio_ProcessPending() from PendSV */
    __DSB();
}

/* Fills one half of the DMA ring and measures how long it took. */
static void audio_fill(int16_t *dst)
{
    const uint32_t frames = AUDIO_BUFFER_FRAMES / 2U;

    /* Load shedding. PendSV preempts the main loop, so a render that takes more than one block period would leave
     * the main loop (USB, MIDI, display) no CPU time at all and the system would look frozen. Output silence
     * for a few blocks instead: the audio drops out, but main gets the CPU time it needs and can recover. */
    if (audio_shed_blocks != 0U)
    {
        audio_shed_blocks--;
        audio_shed_total++;
        memset(dst, 0, frames * 2U * sizeof(int16_t));
        return;
    }

    const uint32_t t0 = DWT->CYCCNT;
    SynthEngine_FillStereoBuffer(dst, frames);
    const uint32_t dt = DWT->CYCCNT - t0;

    const uint32_t budget = (SystemCoreClock / 48000U) * frames;   /* one half-buffer period */
    audio_cyc_last = dt;
    audio_calls++;
    if (dt > audio_cyc_max) {
        audio_cyc_max = dt;
    }
    if (dt > budget - budget / 5U) {
        audio_over80++;
    }
    if (dt > budget) {
        audio_shed_blocks = dt / budget;      /* 1 block for 100..199%, 2 blocks for 200..299%, ... */
        if (audio_over100++ == 0U) {
            /* Last gasp: the main loop is probably starved from now on, so report right here. */
            const uint32_t us = SystemCoreClock / 1000000U;
            Diag_Puts("\n[AUDIO] RENDER OVERRUN: fill took ");
            Diag_PutDec(dt / us);
            Diag_Puts(" us, budget ");
            Diag_PutDec(budget / us);
            Diag_Puts(" us, engine ");
            Diag_Puts(SynthEngine_GetName());
            Diag_Puts("\n");
        }
    }
}

/* Called from PendSV_Handler (stm32h7xx_it.c): renders every half that is waiting. */
void Audio_ProcessPending(void)
{
    unsigned half;
    bool stale;

    while (AudioHalves_Next(&audio_halves, &half, &stale))
    {
        int16_t *dst = &audio_buffer[half * (AUDIO_BUFFER_SIZE / 2U)];

        if (stale)
        {
            /* Both halves were waiting: the older one missed its deadline and the DMA is playing it right now.
             * Silence is better than a repeat of the old block. */
            audio_shed_total++;
            memset(dst, 0, (AUDIO_BUFFER_SIZE / 2U) * sizeof(int16_t));
        }
        else
        {
            audio_fill(dst);
        }
        AudioHalves_Done(&audio_halves, half);
        audio_blocks_done++;
    }
}

/* Main loop: feed the watchdog if (and only if) the audio keeps running. */
static void watchdog_service(void)
{
    static uint32_t last_check_tick = 0;
    static uint32_t last_blocks     = 0;
    const uint32_t now = HAL_GetTick();

    if ((now - last_check_tick) < WATCHDOG_CHECK_MS)
    {
        return;
    }
    last_check_tick = now;

    const uint32_t blocks = audio_blocks_done;
    if (blocks != last_blocks)
    {
        last_blocks = blocks;
        Watchdog_Kick();
    }
}

void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai->Instance == SAI1_Block_A) {
        audio_request_half(0U);
    }
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai->Instance == SAI1_Block_A) {
        audio_request_half(1U);
    }
}

/* Ошибка SAI/DMA (ISR): только счётчик, на дисплей выводит главный цикл */
void HAL_SAI_ErrorCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai->Instance == SAI1_Block_A) {
        sai_error_count++;
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
  Diag_EnterPanic();
  Backup_RecordFault(4U, (uint32_t)__builtin_return_address(0), (uint32_t)__builtin_return_address(0), SCB->CFSR, SCB->HFSR);
  Diag_Puts("\n*** Error_Handler() called, LR=");
  Diag_PutHex((uint32_t)__builtin_return_address(0));
  Diag_Puts(" ***\n");
#ifndef PURRMIDI_FAULT_HALT
  /* Same scheme as the fault handler (Platform/system/diag.c): let the report leave the UART (~1 s; IRQs are off and
   * SysTick is not running, hence a counting loop), then restart. This works at any point of the start-up, also
   * before Watchdog_Start() (MPU_Config, HAL_Init, SystemClock_Config), where the IWDG could not do it. For SWD
   * debugging build with -DPURRMIDI_FAULT_HALT=1: the infinite loop below is kept. */
  for (volatile uint32_t i = 0; i < (SystemCoreClock / 4U); ++i) { __NOP(); }
  NVIC_SystemReset();
#endif
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

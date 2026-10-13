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
#include "synth_engine.h" /* Синтезаторы epiano / pluck / sine / organ / soundfont */
#include "display.h"
#include "midi_event.h"
#include "midi_queue.h"
#include "midi_usb.h"
#include "usb_port_recover.h"
#include "usbh_midi.h"
#include "midi_dispatch.h"
#include "soundfont_synth.h"
#include "sf2_cache.h"
#include "diag.h"
#include "organ_synth.h"
#ifdef PURRMIDI_SF2_XIP
#include "xip_mpu.h"   /* MPU-регионы и D-cache для XIP-чтения SoundFont из QSPI Flash */
#include "sf2_loader.h"
#endif
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

/* Ошибки SAI/DMA: растёт в ISR, главный цикл показывает E02 на дисплее */
static volatile uint32_t sai_error_count = 0;

static uint8_t active_notes[128];
static uint8_t active_note_stack[128];
static uint16_t active_note_count = 0;

/* 1 = print [STATS] and [USBDIAG1..3] to UART (debug only), 0 = silent */
#define USB_DIAG_UART 0

/* 1 = try to load 0:/SNDFNT.SF2 from the SD card once at power-up (progress goes to UART).
 * The SD init / file parsing blocks for a while, so it is done here, not while playing.
 * If it fails, the SoundFont engine is skipped by the Program Change 127 cycle.
 * 0 = never touch the SD card at start-up (the SoundFont engine is then loaded lazily on the
 * first explicit selection with Program Change 4 and may block the main loop for a while). */
#define SF2_LOAD_AT_BOOT 1

/* Some keyboards send the "next instrument" Program Change 127 twice per button press
 * (e.g. on press and on release). Repeats closer than this are ignored. */
#define PC_NEXT_REPEAT_GUARD_MS 300U

/* 1 = enable the Cortex-M7 instruction cache. The code runs from flash, which is slow at 480 MHz
 * without a cache (several wait states per fetch); the organ engine is the heaviest per-sample
 * code in the firmware and ran 3-5x slower than it should. */
#define ENABLE_ICACHE 1

extern uint32_t _sitcm_text;   /* linker symbol: start of the code copied to ITCM */
void OrganProfile_Run(void);   /* Engine/synth/organ_profile.cpp */

/* 16 independent 32-bit loads per pass, 'reps' passes, pure asm so the compiler adds no stack traffic.
 * Returns elapsed CPU cycles. */
#define BENCH_LOADS_BODY                                                                       \
    uint32_t t0 = DWT->CYCCNT;                                                                 \
    __asm volatile(                                                                            \
        "1:\n\t"                                                                               \
        "ldr r2, [%1, #0]\n\t  ldr r3, [%1, #4]\n\t  ldr r4, [%1, #8]\n\t  ldr r5, [%1, #12]\n\t" \
        "ldr r6, [%1, #16]\n\t ldr r8, [%1, #20]\n\t ldr r9, [%1, #24]\n\t ldr r10, [%1, #28]\n\t" \
        "ldr r2, [%1, #32]\n\t ldr r3, [%1, #36]\n\t ldr r4, [%1, #40]\n\t ldr r5, [%1, #44]\n\t" \
        "ldr r6, [%1, #48]\n\t ldr r8, [%1, #52]\n\t ldr r9, [%1, #56]\n\t ldr r10, [%1, #60]\n\t" \
        "subs %0, %0, #1\n\t"                                                                  \
        "bne 1b\n\t"                                                                           \
        : "+r"(reps) : "r"(p) : "r2", "r3", "r4", "r5", "r6", "r8", "r9", "r10", "memory", "cc"); \
    uint32_t dt = DWT->CYCCNT - t0;                                                            \
    *sink = 0;                                                                                 \
    return dt;

/* The same function twice: once linked into ITCM RAM, once in flash. */
__attribute__((noinline, section(".itcm_code")))
static uint32_t bench_loads_itcm(volatile uint32_t *p, uint32_t reps, uint32_t *sink) { BENCH_LOADS_BODY }

__attribute__((noinline))
static uint32_t bench_loads_flash(volatile uint32_t *p, uint32_t reps, uint32_t *sink) { BENCH_LOADS_BODY }

/* 1 = time every audio DMA interrupt with the DWT cycle counter and report it on the UART:
 *   [AUDIO] organ: isr max=NNN us (NN% of 2666 us budget) ...
 * An interrupt that takes longer than its budget (one DMA half-buffer) starves the main loop
 * (USB, MIDI, display) forever - this is how the system "hangs" even though nothing crashed. */
#define AUDIO_DIAG_UART 1

/* 1 = at power-up (before the audio DMA and USB start, so nothing else interferes) time one
 * 128-frame block of the organ and epiano engines with N sounding notes and print the result:
 *   [BENCH] organ  n= 0: 123 us per block
 * This separates "the code itself is slow" from "something steals CPU time while playing". */
#define BOOT_BENCH 1
static volatile uint32_t audio_cyc_max = 0;      /* longest fill so far (CPU cycles) */
static volatile uint32_t audio_cyc_last = 0;
static volatile uint32_t audio_calls = 0;
static volatile uint32_t audio_over80 = 0;       /* fills that used > 80% of the budget */
static volatile uint32_t audio_over100 = 0;      /* fills that used > 100% of the budget */
static volatile bool     audio_reset_stats = false;
static volatile uint32_t audio_shed_blocks = 0;  /* blocks still to be output as silence (CPU load shedding) */
static volatile uint32_t audio_shed_total = 0;   /* blocks dropped so far */
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
/* Перенаправление вывода printf в UART3 */
int _write(int file, char *ptr, int len) {
    (void)file;
    uint32_t timeout = (uint32_t)((len * 10U) / 115U) + 20U;
    if (HAL_UART_Transmit(&huart3, (uint8_t*)ptr, len, timeout) == HAL_OK) {
        return len;
    }
    return 0;
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

/* Названия инструментов для дисплея (только ASCII, до 17 символов). Порядок = SynthEngineId. */
static const char * const display_patch_names[SYNTH_ENGINE_COUNT] =
{
    "E-Piano", "Pluck", "Sine", "Organ", "SoundFont"
};

/* Номер на дисплее 1-based (как на большинстве клавиатур): Program Change 0 -> "01" */
static void Display_SyncPatch(void)
{
    const SynthEngineId id = SynthEngine_GetCurrent();
    Display_SetPatch((uint8_t)(id + 1U), display_patch_names[id]);
}

/* Уровень CC 0..127 -> проценты для полосы громкости */
static void Display_SyncVolume(void)
{
    Display_SetVolume((uint8_t)(((uint32_t)SynthEngine_GetMasterVolume() * 100U + 63U) / 127U));
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
#ifdef PURRMIDI_SF2_XIP
  /* D-cache нужен для быстрого XIP-чтения сэмплов из QSPI Flash. Все SRAM с DMA-буферами (AXI, SRAM1-3, SRAM4)
   * объявляются non-cacheable, кэшируется только окно 0x90000000 (оно открывается драйвером QSPI). */
  XipMpu_Config();
  SCB_EnableDCache();
#endif

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
  MX_QUADSPI_Init();
  /* USER CODE BEGIN 2 */
  Display_Init();
  MIDI_Queue_Init();
  MIDI_USB_Init();
  // SDStorage_Mount()
  SynthEngine_Init(); // Включает синтезатор по умолчанию (E-Piano); дальше выбор — по Program Change
  printf("Synth engine: %s\r\n", SynthEngine_GetName());
  Display_SyncPatch();
  Display_SyncVolume();
  printf("[SYS] organ code at %p (0x0000xxxx = ITCM, 0x0800xxxx = flash), sample table at %p\r\n",
         (void *)OrganSynth_FillStereoBuffer, (void *)&_sitcm_text);
  printf("[SYS] SYSCLK=%lu Hz, I-cache %s, audio budget %lu us per %u frames\r\n",
         (unsigned long)SystemCoreClock, ENABLE_ICACHE ? "ON" : "off",
         (unsigned long)(1000000UL * (AUDIO_BUFFER_FRAMES / 2U) / 48000UL), (unsigned)(AUDIO_BUFFER_FRAMES / 2U));
#if SF2_LOAD_AT_BOOT
#ifdef PURRMIDI_SF2_XIP
  /* Банк читается из QSPI Flash (XIP). Если вставлена SD-карта с новым/изменённым 0:/SNDFNT.SF2, он сначала
   * переписывается во флеш (до ~минуты, прогресс на дисплее и в UART), см. docs/QSPI_XIP.md. */
  printf("[SF2] Preparing SoundFont (QSPI XIP, SD is used only to update the bank)...\r\n");
#else
  printf("[SF2] Loading SoundFont from SD...\r\n");
#endif
  if (SoundFontSynth_InitSF2())
  {
    printf("[SF2] SoundFont ready\r\n");
  }
  else
  {
    printf("[SF2] SoundFont NOT available - the soundfont engine will be skipped\r\n");
  }
#endif
#if BOOT_BENCH
  {
    static int16_t bench_buf[256];
    static const uint8_t bench_notes[] = { 48, 52, 55, 60, 64, 67, 72, 76, 79, 84 };
    const uint32_t us = SystemCoreClock / 1000000U;
    const SynthEngineId bench_engines[2] = { SYNTH_ENGINE_ORGAN, SYNTH_ENGINE_EPIANO };
    const char *bench_names[2] = { "organ ", "epiano" };
    const int bench_counts[] = { 0, 1, 3, 6, 10 };

    /* Reference: a loop of exactly 1M iterations (2M instructions: subs + bne). A healthy
     * Cortex-M7 needs about 1.0-1.1M cycles. Much more = the CPU is being slowed down
     * (interrupt storm, bus contention, memory attributes). Done with IRQs on and off. */
    for (int irq_off = 0; irq_off < 2; irq_off++)
    {
      uint32_t n = 1000000U;
      if (irq_off) { __disable_irq(); }
      uint32_t t0 = DWT->CYCCNT;
      __asm volatile("1: subs %0, %0, #1\n\t bne 1b" : "+r"(n));
      uint32_t dt = DWT->CYCCNT - t0;
      if (irq_off) { __enable_irq(); }
      printf("[BENCH] spin 1M iterations, IRQ %s: %lu cycles\r\n", irq_off ? "OFF" : "on ", (unsigned long)dt);
    }

    /* Where do data accesses go and how fast are they? 1M dependent 32-bit loads from each memory.
     * Zero-wait TCM = about 1-2 cycles per load. Tens of cycles per load = the CPU reaches that
     * memory over a slow bus path (TCM interface disabled, wrong memory attributes, ...). */
    printf("[BENCH] SCB->ITCMCR=0x%08lX DTCMCR=0x%08lX CCR=0x%08lX CACR=0x%08lX\r\n",
           (unsigned long)SCB->ITCMCR, (unsigned long)SCB->DTCMCR, (unsigned long)SCB->CCR,
           (unsigned long)SCB->CACR);
    {
      static const struct { const char *name; uint32_t addr; } regions[] = {
        { "ITCM   0x00000400", 0x00000400UL },
        { "DTCM   0x2001C000", 0x2001C000UL },
        { "AXI    0x24060000", 0x24060000UL },
        { "RAM_D2 0x30020000", 0x30020000UL },
        { "FLASH  0x08010000", 0x08010000UL },
      };
      for (unsigned r = 0; r < sizeof(regions) / sizeof(regions[0]); r++)
      {
        volatile uint32_t *p = (volatile uint32_t *)regions[r].addr;
        uint32_t sum = 0;
        if (regions[r].addr >= 0x20000000UL)
        {
          /* RAM has ECC: initialise the test words (full-word writes) before reading them. */
          for (uint32_t i = 0; i < 256U; i++) { p[i] = i; }
        }
        uint32_t t0 = DWT->CYCCNT;
        for (uint32_t i = 0; i < 100000U; i++)
        {
          sum += p[i & 0xFFU];       /* 1 KB working set, 4-byte stride */
        }
        uint32_t dt = DWT->CYCCNT - t0;
        printf("[BENCH] 100k loads from %s: %lu cycles (%lu.%02lu per load) sum=%lu\r\n", regions[r].name,
               (unsigned long)dt, (unsigned long)(dt / 100000U), (unsigned long)((dt / 1000U) % 100U),
               (unsigned long)sum);
      }
    }

    OrganProfile_Run();
    if (SoundFontSynth_IsLoaded())
    {
      SoundFontSynth_Profile();
    }

    /* Straight-line load benchmark: separates the cost of the LOAD from the cost of FETCHING the code.
     * rows = where the code runs, columns = where the data is. Value = CPU cycles per load. */
    {
      static const struct { const char *name; uint32_t addr; } data_regions[] = {
        { "DTCM", 0x2001C000UL }, { "AXI", 0x24060000UL }, { "FLASH", 0x08010000UL },
      };
      const uint32_t reps = 5000U;
      uint32_t sink = 0;
      printf("[BENCH2] cycles per 32-bit load (16 loads x %lu passes)\r\n", (unsigned long)reps);
      for (int mode = 0; mode < 3; mode++)
      {
        const char *code_name = (mode == 0) ? "code in ITCM         " :
                                (mode == 1) ? "code in flash, ICache" : "code in flash, no IC ";
        if (mode == 2) { SCB_DisableICache(); }
        printf("[BENCH2] %s:", code_name);
        for (unsigned r = 0; r < 3; r++)
        {
          volatile uint32_t *p = (volatile uint32_t *)data_regions[r].addr;
          if (data_regions[r].addr >= 0x20000000UL)
          {
            for (uint32_t i = 0; i < 16U; i++) { p[i] = i; }
          }
          uint32_t dt = (mode == 0) ? bench_loads_itcm(p, reps, &sink) : bench_loads_flash(p, reps, &sink);
          uint32_t x100 = (uint32_t)(((uint64_t)dt * 100U) / (16U * reps));
          printf("  %s=%lu.%02lu", data_regions[r].name, (unsigned long)(x100 / 100U), (unsigned long)(x100 % 100U));
        }
        printf("\r\n");
        if (mode == 2) { SCB_EnableICache(); }
      }
      (void)sink;
    }

    for (int e = 0; e < 2; e++)
    {
      for (unsigned c = 0; c < sizeof(bench_counts) / sizeof(bench_counts[0]); c++)
      {
        SynthEngine_Select(SYNTH_ENGINE_SINE);               /* Select() of the current engine is a no-op, */
        SynthEngine_Select(bench_engines[e]);                /* so switch away and back to re-initialise it */
        for (int k = 0; k < bench_counts[c]; k++)
        {
          SynthEngine_NoteOn(bench_notes[k], 100);
        }
        SynthEngine_FillStereoBuffer(bench_buf, 128);        /* applies the events */
        uint32_t t0 = DWT->CYCCNT;
        SynthEngine_FillStereoBuffer(bench_buf, 128);
        SynthEngine_FillStereoBuffer(bench_buf, 128);
        SynthEngine_FillStereoBuffer(bench_buf, 128);
        uint32_t dt = (DWT->CYCCNT - t0) / 3U;
        /* the same again with interrupts masked */
        __disable_irq();
        t0 = DWT->CYCCNT;
        SynthEngine_FillStereoBuffer(bench_buf, 128);
        SynthEngine_FillStereoBuffer(bench_buf, 128);
        SynthEngine_FillStereoBuffer(bench_buf, 128);
        uint32_t dt_noirq = (DWT->CYCCNT - t0) / 3U;
        __enable_irq();
        printf("[BENCH] %s n=%2d: %lu us per 128-frame block (%lu cycles), IRQ masked: %lu cycles\r\n",
               bench_names[e], bench_counts[c], (unsigned long)(dt / us), (unsigned long)dt,
               (unsigned long)dt_noirq);
      }
    }
    SynthEngine_Select(SYNTH_ENGINE_EPIANO);
    Display_SyncPatch();
  }
#endif
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

  uint32_t seen_queue_overruns = MIDI_Queue_GetOverrunCount();
  uint32_t seen_rx_errors      = MIDI_USB_GetReceiveErrorsCount();
  uint32_t seen_sai_errors     = sai_error_count;
  uint32_t seen_audio_shed     = audio_shed_total;
  bool     sf2_missing_shown   = false;

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
    Button_K1_Process();
    USB_PortLostRecover(&hUsbHostFS);   /* port silently disabled by HW -> re-enumerate */
    MIDI_USB_Process();
    SoundFontSynth_Process();

    if (MIDI_USB_HasStateChanged())
    {
        Display_SetMidiConnected(MIDI_USB_IsConnected());
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
                printf("[PC] ch=%u program 127 repeat ignored (+%lu ms)\r\n",
                       event.status & 0x0FU, (unsigned long)(pc_now - pc_next_tick));
                continue;
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
        }
        else if (command == MIDI_STATUS_CONTROL_CHANGE)
        {
            if (event.data1 == SYNTH_MASTER_VOLUME_CC)
            {
                Display_SyncVolume();
            }
            else if (event.data1 == 64U)
            {
                Display_SetSustain(event.data2 >= 64U);
            }
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
            Display_SetVelocity(event.data2);
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

#if AUDIO_DIAG_UART
    {
        /* Report only when a new maximum is reached (or the budget was exceeded), so the log stays quiet. */
        static uint32_t last_audio_report = 0;
        static uint32_t reported_max = 0;
        static uint32_t reported_over80 = 0;
        static uint32_t reported_shed = 0;
        static uint32_t last_sf2_print = 0;
        static uint32_t sf2_last_calls = 0;
        uint32_t t_now = HAL_GetTick();

        if (audio_reset_stats)
        {
            audio_reset_stats = false;
            SF2Cache_ResetProfile();
            sf2_last_calls = 0;
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
            if (m > reported_max + reported_max / 8U || o80 != reported_over80 || audio_shed_total != reported_shed)
            {
                const uint32_t us = SystemCoreClock / 1000000U;
                const uint32_t budget = (SystemCoreClock / 48000U) * (AUDIO_BUFFER_FRAMES / 2U);
                printf("[AUDIO] %s: isr last=%lu us max=%lu us (%lu%% of %lu us) >80%%: %lu >100%%: %lu shed=%lu calls=%lu\r\n",
                       SynthEngine_GetName(),
                       (unsigned long)(audio_cyc_last / us), (unsigned long)(m / us),
                       (unsigned long)(m / (budget / 100U)), (unsigned long)(budget / us),
                       (unsigned long)o80, (unsigned long)audio_over100, (unsigned long)audio_shed_total,
                       (unsigned long)audio_calls);
                reported_max = m;
                reported_over80 = o80;
                reported_shed = audio_shed_total;
            }
        }

        /* SoundFont cache statistics: at most once a second, only while the cache is being read. */
        if (SynthEngine_GetCurrent() == SYNTH_ENGINE_SOUNDFONT && (t_now - last_sf2_print) >= 1000U)
        {
            SF2Profile sp;
            SF2Cache_GetProfile(&sp);
            if (sp.calls != sf2_last_calls)
            {
                sf2_last_calls = sp.calls;
                last_sf2_print = t_now;
                SoundFontSynth_PrintStats();
            }
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

        /* Load shedding: блоки заменены тишиной, т.е. звук реально пропал (счётчик не сбрасывается) */
        const uint32_t shed = audio_shed_total;
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

/* Fills one half of the DMA ring and measures how long it took. */
static void audio_fill(int16_t *dst)
{
    const uint32_t frames = AUDIO_BUFFER_FRAMES / 2U;

    /* Load shedding. If a fill took longer than one block period, the next DMA interrupt is already
     * pending when this one returns, so the main loop (USB, MIDI, SD/SoundFont cache loader, display)
     * would never run again and the system would look frozen. Output silence for a few blocks instead:
     * the audio drops out, but main gets the CPU time it needs and can recover. */
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
            Diag_Puts("\n[AUDIO] ISR OVERRUN: fill took ");
            Diag_PutDec(dt / us);
            Diag_Puts(" us, budget ");
            Diag_PutDec(budget / us);
            Diag_Puts(" us, engine ");
            Diag_Puts(SynthEngine_GetName());
            Diag_Puts("\n");
        }
    }
}

void HAL_SAI_TxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai->Instance == SAI1_Block_A) {
        audio_fill(&audio_buffer[0]);
    }
}

void HAL_SAI_TxCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai->Instance == SAI1_Block_A) {
        audio_fill(&audio_buffer[AUDIO_BUFFER_SIZE / 2]);
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
  Diag_Puts("\n*** Error_Handler() called, LR=");
  Diag_PutHex((uint32_t)__builtin_return_address(0));
  Diag_Puts(" ***\n");
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

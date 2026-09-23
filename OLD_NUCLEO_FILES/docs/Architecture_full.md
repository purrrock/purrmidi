# 1. Архитектура PurrMidi

Получается следующая структура:

```text
USB-C / USB FS
PA11 USB_DM
PA12 USB_DP
        │
        ▼
USB OTG FS Host
        │
        ▼
MIDI parser / event queue
        │
        ▼
Software Synth
        │
        ▼
SAI1 Block B
PF8  BCLK
PF9  LRCK
PF6  SD
        │
        ▼
PCM5102A
        │
        ▼
Audio
```

Параллельно:

```text
ST7735
SPI4
PE12 SCK
PE14 MOSI
PE11 CS
PE13 DC
PE10 BL

microSD
SDMMC1 4-bit

8 MB QSPI
QUADSPI

8 MB SPI Flash
SPI1

diagnostic UART
USART3
PB10 TX
PB11 RX
```

Это оставляет достаточно GPIO для будущих кнопок/энкодера/LED.

---

# 2. Итоговая PurrMidi pinout table

## Основные периферийные функции

| MCU pin  | Функция MCU                 | Функция на плате WeAct       | Использование PurrMidi        | Примечание                                      |
| -------- | --------------------------- | ---------------------------- | ----------------------------- | ----------------------------------------------- |
| **PA11** | USB_OTG_FS_DM               | USB-C D−                     | **USB Host D−**               | Занят USB                                       |
| **PA12** | USB_OTG_FS_DP               | USB-C D+                     | **USB Host D+**               | Занят USB                                       |
| **PA10** | USB_OTG_FS_ID / USART1_RX   | USB ID                       | **USB Host / reserved**       | Не использовать под UART                        |
| **PA9**  | USB_OTG_FS_VBUS / USART1_TX | USB VBUS-related / boot UART | **USB VBUS sense / reserved** | Не использовать под диагностический UART        |
| **PB10** | USART3_TX                   | Выведен на гребёнку          | **Diagnostic UART TX**        | Рекомендовано                                   |
| **PB11** | USART3_RX                   | Выведен на гребёнку          | **Diagnostic UART RX**        | Рекомендовано                                   |
| **PF8**  | SAI1_SCK_B                  | Свободный GPIO               | **PCM5102A BCLK**             | Audio                                           |
| **PF9**  | SAI1_FS_B                   | Свободный GPIO               | **PCM5102A LRCK/WS**          | Audio                                           |
| **PF6**  | SAI1_SD_B                   | Свободный GPIO               | **PCM5102A DIN/SD**           | Audio                                           |
| **PF7**  | SAI1_MCLK_B                 | Свободный GPIO               | Не используется               | MCLK PCM5102A не нужен                          |
| **PE12** | SPI4_SCK                    | ST7735 SCK                   | **TFT SCK**                   | Занят LCD                                       |
| **PE14** | SPI4_MOSI                   | ST7735 MOSI                  | **TFT MOSI**                  | Занят LCD                                       |
| **PE13** | SPI4_MISO                   | **ST7735 DC/RS**             | **TFT DC**                    | Важное отличие: физически это не свободный MISO |
| **PE11** | SPI4_NSS                    | ST7735 CS                    | **TFT CS**                    | Занят LCD                                       |
| **PE10** | GPIO                        | ST7735 backlight             | **TFT BL**                    | Active-low по WeAct                             |
| **PC8**  | SDMMC1_D0                   | microSD D0                   | **SD D0**                     | 4-bit SDMMC                                     |
| **PC9**  | SDMMC1_D1                   | microSD D1                   | **SD D1**                     | 4-bit SDMMC                                     |
| **PC10** | SDMMC1_D2                   | microSD D2                   | **SD D2**                     | 4-bit SDMMC                                     |
| **PC11** | SDMMC1_D3                   | microSD D3                   | **SD D3**                     | 4-bit SDMMC                                     |
| **PC12** | SDMMC1_CK                   | microSD CLK                  | **SD CLK**                    | 4-bit SDMMC                                     |
| **PD2**  | SDMMC1_CMD                  | microSD CMD                  | **SD CMD**                    | Занят SD                                        |
| **PD4**  | GPIO                        | microSD card detect          | **SD card detect**            | Pull-up                                         |
| **PB2**  | QUADSPI_CLK                 | 8 MB QSPI Flash              | **QSPI CLK**                  | Занят Flash                                     |
| **PB6**  | QUADSPI_BK1_NCS             | 8 MB QSPI Flash              | **QSPI CS**                   | Занят Flash                                     |
| **PD11** | QUADSPI_BK1_IO0             | 8 MB QSPI Flash              | **QSPI IO0**                  | Занят Flash                                     |
| **PD12** | QUADSPI_BK1_IO1             | 8 MB QSPI Flash              | **QSPI IO1**                  | Занят Flash                                     |
| **PE2**  | QUADSPI_BK1_IO2             | 8 MB QSPI Flash              | **QSPI IO2**                  | Занят Flash                                     |
| **PD13** | QUADSPI_BK1_IO3             | 8 MB QSPI Flash              | **QSPI IO3**                  | Занят Flash                                     |
| **PB3**  | SPI1_SCK                    | 8 MB SPI Flash               | **SPI Flash SCK**             | Занят Flash                                     |
| **PB4**  | SPI1_MISO                   | 8 MB SPI Flash               | **SPI Flash MISO**            | Занят Flash                                     |
| **PD7**  | SPI1_MOSI                   | 8 MB SPI Flash               | **SPI Flash MOSI**            | Занят Flash                                     |
| **PD6**  | SPI1_NSS                    | 8 MB SPI Flash CS            | **SPI Flash CS**              | Занят Flash                                     |

WeAct прямо определяет SPI Flash как 64 Mbit и использует `PD6/PB3/PB4/PD7`; QSPI Flash также 64 Mbit, с `PB6/PB2/PD11/PD12/PE2/PD13`. [WeAct board configuration](https://github.com/WeActStudio/MiniSTM32H7xx/blob/master/SDK/openmv/Ports/Below%20V4.4.1/micropython/boards/WeActStudioSTM32H7xx/mpconfigboard.h?utm_source=chatgpt.com)

SDMMC1 у WeAct определён именно как `PC8..PC12 + PD2`, а detect — `PD4`. [WeAct SDMMC example](https://github.com/WeActStudio/MiniSTM32H7xx/tree/master/SDK/HAL/STM32H743/04-SD_Test?utm_source=chatgpt.com)

---

# 3. Встроенные устройства платы

## ST7735

WeAct board configuration даёт:

```text
PE12 = SPI4_SCK
PE14 = SPI4_MOSI
PE13 = LCD_RS/DC
PE11 = LCD_CS
PE10 = LCD_BL
```

Причём особенно важный момент:

**PE13 нельзя считать обычным SPI4_MISO.**

Хотя MCU умеет:

```text
PE13 = SPI4_MISO
```

на конкретной плате этот вывод используется как **LCD_RS/DC**.

RESET LCD в конфигурации WeAct не определён:

```text
OMV_SPI_LCD_RST_PIN // not connected
```

То есть драйвер ST7735 должен исходить из того, что аппаратного RESET GPIO у встроенного дисплея нет. [WeAct LCD board configuration](https://github.com/WeActStudio/MiniSTM32H7xx/blob/master/SDK/openmv/Ports/Below%20V4.4.1/omv/boards/WeActStudioSTM32H7xx/omv_boardconfig.h?utm_source=chatgpt.com)

---

# 4. USB Host — важный нюанс

Для USB MIDI Host аппаратные data lines правильные:

```text
PA11 → USB D-
PA12 → USB D+
```

Именно их WeAct использует для USB FS. [WeAct USB configuration](https://github.com/WeActStudio/MiniSTM32H7xx/blob/master/SDK/openmv/Ports/micropython/boards/WeActStudioSTM32H7xx/pins.csv?utm_source=chatgpt.com)

### PA10

WeAct определяет:

```text
USB_ID = PA10
```

Поэтому PA10 я **не назначаю USART1 RX**, хотя такая функция MCU существует.

### PA9

PA9 — кандидат на:

```text
USB_OTG_FS_VBUS
```

но WeAct в своей конфигурации VBUS sensing оставляет выключенным:

```text
//#define MICROPY_HW_USB_VBUS_DETECT_PIN (pin_A9)
```

и одновременно использует PA9/PA10 как UART1 для bootloader/сервисного подключения. [WeAct board configuration](https://github.com/WeActStudio/MiniSTM32H7xx/blob/master/SDK/openmv/Ports/Below%20V4.4.1/micropython/boards/WeActStudioSTM32H7xx/mpconfigboard.h?utm_source=chatgpt.com)

### Поэтому для PurrMidi

Я предлагаю:

```text
PA9  = RESERVED USB/VBUS
PA10 = RESERVED USB/ID
PA11 = USB DM
PA12 = USB DP
```

а диагностический UART перенести на:

```text
PB10 = USART3_TX
PB11 = USART3_RX
```

Это существенно чище.

### Но USB Host требует ещё VBUS 5 V

Это отдельный вопрос от USB D+/D−.

Для клавиатуры:

```text
STM32 → USB Host
```

нужно, чтобы USB-разъём, к которому подключается клавиатура, **подавал 5 V на VBUS как USB host**.

По доступным данным WeAct плата допускает питание от USB VBUS и имеет USB-C, но физическую схему переключения/источника host VBUS я не буду угадывать. ([GitHub][4])

**Перед финальной сборкой PurrMidi обязательно проверить мультиметром:**

1. куда приходит VBUS USB-C;
2. соединён ли он непосредственно с 5-V rail;
3. может ли плата безопасно быть источником 5 V для подключённой MIDI-клавиатуры;
4. есть ли current-limit/load switch;
5. что происходит с VBUS при питании платы от внешних 5 V.

Если окажется, что USB-C на плате рассчитан только на **device/power input**, для Host понадобится внешний USB Host VBUS switch/питание.

---

# 5. SAI1_B → PCM5102A

Это ключевой выбор.

У H743 есть несколько вариантов SAI, но нам не нужно занимать PE4–PE6.

## Выбранный вариант

```text
SAI1 Block B

PF8  SAI1_SCK_B → PCM5102A BCK
PF9  SAI1_FS_B  → PCM5102A LRCK
PF6  SAI1_SD_B  → PCM5102A DIN
```

### MCLK

Не подключаем:

```text
PF7 SAI1_MCLK_B → unused
```

PCM5102A имеет внутреннюю audio PLL и допускает **3-wire I²S: BCK + LRCK + DIN**, без внешнего MCLK. ([Texas Instruments][1])

Это здесь очень удобно: мы экономим один GPIO и не занимаем PF7.

### Почему не SAI1_A?

SAI1_A:

```text
PE2  MCLK
PE4  FS
PE5  SCK
PE6  SD
```

Но:

```text
PE2 → QSPI Flash IO2
```

а:

```text
PE4/PE5/PE6
```

используются интерфейсом DCMI камеры платы.

Поэтому SAI1_A создаёт лишнюю конкуренцию за аппаратные ресурсы платы.

SAI1_B:

```text
PF6
PF8
PF9
```

не используется встроенными PurrMidi-периферийными устройствами WeAct.

Именно поэтому **SAI1_B — предпочтительный вариант**. Альтернативные функции PF6–PF9 подтверждаются таблицей AF STM32H743. ([Платан][5])

---

# 6. DMA для SAI

Для первого варианта предлагаю:

```text
SAI1 Block B TX
    ↓
DMA2 Stream 4
    ↓
DMA_REQUEST_SAI1_B
```

У ST есть такой же вариант для SAI1 Block B в H743 BSP:

```text
DMA2_Stream4
DMA_REQUEST_SAI1_B
```

([GitHub][6])

### Важное замечание про TFT

В WeAct/OpenMV для SPI4 TX также используется DMA2 Stream4. Поэтому мы **не должны одновременно** сделать:

```text
SAI1_B TX → DMA2 Stream4
SPI4 TX    → DMA2 Stream4
```

с одновременной работой обоих DMA.

Для PurrMidi предлагаю:

**SAI audio получает DMA2 Stream4 как приоритетный ресурс.**

TFT на первом этапе:

```text
SPI4 + blocking/polling TX
```

или позднее перенести SPI4 TX на другой совместимый DMA stream.

Для MIDI-синтезатора это разумнее: audio DMA должен иметь предсказуемый ресурс.

---

# 7. SD card

Используем аппаратный:

```text
SDMMC1
4-bit wide bus
```

```text
PC8  D0
PC9  D1
PC10 D2
PC11 D3
PC12 CK
PD2  CMD
PD4  Card Detect
```

Это ровно та конфигурация, которую использует WeAct в своём SD example. [WeAct STM32H743 SD example](https://github.com/WeActStudio/MiniSTM32H7xx/tree/master/SDK/HAL/STM32H743/04-SD_Test?utm_source=chatgpt.com)

Для PurrMidi это удобно для:

* SoundFont/sample data;
* пресетов;
* будущих wav/PCM samples;
* файловой системы.

---

# 8. Две Flash-памяти

## QSPI Flash — 8 MB

```text
PB2  CLK
PB6  CS
PD11 IO0
PD12 IO1
PE2  IO2
PD13 IO3
```

Это **не свободные GPIO**.

## SPI Flash — 8 MB

```text
PB3  SCK
PB4  MISO
PD7  MOSI
PD6  CS
```

Также считаем их занятыми.

WeAct board configuration указывает обе Flash как 64 Mbit = 8 MB. [WeAct Flash definitions](https://github.com/WeActStudio/MiniSTM32H7xx/blob/master/SDK/openmv/Ports/Below%20V4.4.1/micropython/boards/WeActStudioSTM32H7xx/mpconfigboard.h?utm_source=chatgpt.com)

---

# 9. SWD, кварцы, кнопки, LED

| MCU pin   | Что подключено     | PurrMidi                                |
| --------- | ------------------ | --------------------------------------- |
| **PA13**  | SWDIO              | SWD                                     |
| **PA14**  | SWCLK              | SWD                                     |
| **NRST**  | Reset              | SWD/reset                               |
| **BOOT0** | Boot selector      | Bootloader                              |
| **PC14**  | 32.768 kHz crystal | RTC oscillator                          |
| **PC15**  | 32.768 kHz crystal | RTC oscillator                          |
| **PH0**   | 25 MHz HSE         | Main clock                              |
| **PH1**   | 25 MHz HSE         | Main clock                              |
| **PC13**  | User button        | Можно использовать как системную кнопку |
| **PE3**   | Blue LED           | Можно использовать как diagnostic LED   |

WeAct указывает 25 MHz high-speed crystal и 32.768 kHz low-speed crystal; также имеются User Key PC13 и NRST/BOOT0 keys. ([GitHub][4])

PE3 официально используется WeAct как blue LED. [WeAct LED definition](https://github.com/WeActStudio/MiniSTM32H7xx/blob/master/SDK/openmv/Ports/micropython/boards/WeActStudioSTM32H7xx/mpconfigboard.h?utm_source=chatgpt.com)

---

# 10. Полностью занятые выводы платы

Я бы разделил их на две категории.

## A. Не использовать вообще

```text
PA9     USB VBUS / reserved
PA10    USB ID
PA11    USB DM
PA12    USB DP

PA13    SWDIO
PA14    SWCLK

PC13    User Button
PC14    LSE
PC15    LSE

PH0     HSE
PH1     HSE
NRST
BOOT0

PE2     QSPI
PE3     LED

PE10    TFT BL
PE11    TFT CS
PE12    TFT SCK
PE13    TFT DC
PE14    TFT MOSI

PB2     QSPI CLK
PB3     SPI Flash SCK
PB4     SPI Flash MISO
PB6     QSPI CS
PD6     SPI Flash CS
PD7     SPI Flash MOSI

PD11    QSPI IO0
PD12    QSPI IO1
PD13    QSPI IO3

PC8     SD D0
PC9     SD D1
PC10    SD D2
PC11    SD D3
PC12    SD CLK
PD2     SD CMD
PD4     SD Detect
```

## B. Зарезервировать под PurrMidi

```text
PF6     SAI1_B SD
PF8     SAI1_B SCK
PF9     SAI1_B FS

PB10    USART3 TX
PB11    USART3 RX
```

---

# 11. DCMI — формально не нужен PurrMidi, но учитывать

У платы есть camera connector. WeAct использует:

```text
PA4  DCMI_HSYNC
PA6  DCMI_PIXCLK

PC6  DCMI_D0
PC7  DCMI_D1

PE0  DCMI_D2
PE1  DCMI_D3
PE4  DCMI_D4
PD3  DCMI_D5
PE5  DCMI_D6
PE6  DCMI_D7

PB7  DCMI_VSYNC
```

Это подтверждается их DCMI example. [WeAct DCMI example](https://github.com/WeActStudio/MiniSTM32H7xx/tree/master/SDK/HAL/STM32H743/08-DCMI2LCD?utm_source=chatgpt.com)

Для **PurrMidi камера не нужна**, поэтому эти GPIO можно считать свободными с точки зрения текущей аппаратуры.

Но я всё же рекомендую **не использовать PE4/PE5/PE6**, поскольку именно они образуют очень удобный альтернативный SAI1_A. Оставляем возможность в будущем использовать camera connector.

---

# 12. Свободные GPIO для PurrMidi

После фиксации основной архитектуры остаётся достаточно свободных GPIO.

## Первая группа — рекомендую для кнопок

```text
PA0
PA1
PA2
PA3
```

Причём PA2/PA3 также являются:

```text
USART2_TX
USART2_RX
```

Поэтому:

* если нужен второй UART — оставить PA2/PA3;
* если UART2 не нужен — отличные GPIO для кнопок.

## Хорошие GPIO

```text
PB0
PB1

PC4
PC5

PE7
PE8
PE9
PE15

PF0
PF1
PF2
PF3
PF4
PF5
PF10
```

### Для энкодера

Я бы предварительно выделил:

```text
PA0
PA1
```

или:

```text
PB0
PB1
```

Но окончательно таймеры для энкодера выберем уже после того, как определимся с UX.

### Для дополнительных LED

```text
PC4
PC5
PE15
PF0...
```

подходят как обычные GPIO.

### Уже имеющийся LED

```text
PE3
```

лучше оставить системным:

```text
USB status
MIDI activity
audio underrun
error
```

---

# 13. Итоговая карта ресурсов PurrMidi

```text
                    STM32H743VIT6
                         │
 ┌───────────────────────┼────────────────────────┐
 │                       │                        │
USB Host                Audio                   Storage
 │                       │                        │
PA11/PA12              SAI1_B                  SDMMC1
PA9/PA10 reserved      PF8 BCLK                PC8-12
 │                     PF9 LRCK                PD2
 │                     PF6 DATA                PD4
 │
MIDI                    PCM5102A
 │
USB Host
 │
Parser
 │
Queue
 │
Synth
```

Параллельно:

```text
SPI4
 └── ST7735

SPI1
 └── 8 MB SPI Flash

QUADSPI
 └── 8 MB QSPI Flash

USART3
 └── diagnostic UART

SWD
 └── debugger
```

---

# 14. CubeMX — базовая конфигурация

## MCU

Выбрать:

```text
STM32H743VIT6
LQFP100
```

Не использовать generic H743 board preset.

---

## RCC

У WeAct:

```text
HSE = 25 MHz
LSE = 32.768 kHz
```

25 MHz HSE подтверждается и исходниками WeAct. [WeAct clock configuration](https://github.com/WeActStudio/MiniSTM32H7xx/blob/master/SDK/HAL/STM32H743/01-GPIO/01-GPIO.ioc?utm_source=chatgpt.com)

Для PurrMidi я бы ориентировался на:

```text
CPU = 480 MHz
```

а USB:

```text
48 MHz
```

от корректного USB clock source.

---

# 15. SYS

```text
Debug = Serial Wire
```

получаем:

```text
PA13 = SWDIO
PA14 = SWCLK
```

---

# 16. USB

В CubeMX:

```text
USB_OTG_FS
Mode = Host Only
```

GPIO:

```text
PA11 = USB_OTG_FS_DM
PA12 = USB_OTG_FS_DP
```

После этого:

```text
Middleware
    USB_HOST
```

и Host class пока можно оставить минимальным/без MIDI-класса.

На следующем этапе мы будем работать уже с raw USB MIDI device.

---

# 17. USART3

```text
USART3
Mode = Asynchronous
TX = PB10
RX = PB11
```

Например:

```text
115200
8N1
```

Это будет основной diagnostic UART PurrMidi.

---

# 18. SPI4 / TFT

```text
SPI4
Mode = Full-Duplex Master
```

но фактически LCD использует только:

```text
SCK  PE12
MOSI PE14
```

и GPIO:

```text
PE11 CS
PE13 DC
PE10 BL
```

MISO:

```text
PE13
```

не использовать как MISO — он физически занят LCD DC.

Для первой реализации я бы **не включал DMA для SPI4**.

---

# 19. SDMMC

```text
SDMMC1
4-bit Wide Bus
```

CubeMX должен получить:

```text
PC8  D0
PC9  D1
PC10 D2
PC11 D3
PC12 CK
PD2  CMD
```

Card Detect:

```text
PD4 GPIO Input
Pull-up
```

Далее:

```text
Middleware → FatFs
```

---

# 20. QSPI

```text
QUADSPI
Single Flash
1-1-4 / Quad SPI
```

GPIO:

```text
PB2
PB6
PD11
PD12
PE2
PD13
```

Параметры конкретного W25Qxx нужно будет согласовать с установленной на конкретной плате микросхемой, но **pinout менять нельзя**.

---

# 21. SPI1 Flash

```text
SPI1 Master
```

```text
PB3 SCK
PB4 MISO
PD7 MOSI
PD6 CS GPIO
```

CS лучше оставить обычным GPIO, а не обязательно аппаратным NSS.

---

# 22. SAI1

В CubeMX:

```text
SAI1
Block B
Audio Frequency:
    48 kHz
```

Первоначально:

```text
Master TX
I2S / Philips
Stereo
32 bit
```

GPIO:

```text
PF8 = SAI1_SCK_B
PF9 = SAI1_FS_B
PF6 = SAI1_SD_B
```

MCLK:

```text
Disabled
```

PCM5102A это поддерживает. ([Texas Instruments][1])

Для первого запуска я бы использовал:

```text
48 kHz
32-bit container
stereo
DMA circular
```

---

# 23. DMA SAI

В CubeMX:

```text
SAI1_B_TX
DMA2_Stream4
Request = SAI1_B
Circular
Memory increment = enabled
Peripheral increment = disabled
```

Для audio buffer:

```text
HalfWord
```

или соответствующее выравнивание в зависимости от выбранного SAI data size.

**DMA2 Stream4 резервируем за audio.**

---

# 24. H743 RAM — здесь нельзя повторить F411-подход

Это важнее самой распиновки.

У H743:

```text
ITCM RAM      64 KB
DTCM RAM     128 KB

AXI SRAM     512 KB

D2 SRAM1     128 KB
D2 SRAM2     128 KB
D2 SRAM3      32 KB

D3 SRAM4      64 KB
```

Reference Manual прямо указывает, что SRAM1/2 в D2 подходят для DMA buffers, SRAM3 — в частности для USB, а DTCM доступен CPU и MDMA, но не обычным DMA1/DMA2. ([STMicroelectronics][3])

---

# 25. Где размещать PurrMidi buffers

Я бы сразу зафиксировал архитектуру:

### Audio DMA buffers

```text
SRAM1 / SRAM2
0x30000000
0x30020000
```

Например:

```text
audio_dma_buffer[]
```

**Не DTCM.**

Идеальный вариант:

```text
SRAM1/2
MPU = non-cacheable
```

Тогда DMA и CPU видят одни и те же данные без ручного cache maintenance.

---

### MIDI event queue

```text
DTCM
0x20000000
```

например:

```text
midi_event_queue[]
```

Поскольку MIDI queue не передаётся DMA, DTCM здесь очень удобен.

Преимущество:

* zero wait-state;
* deterministic CPU access;
* не нужно заниматься D-cache coherency.

---

### USB buffers

Для USB я бы зарезервировал:

```text
SRAM3
0x30040000
```

Это как раз память, которую ST отдельно рекомендует для USB/peripheral buffers. ([STMicroelectronics][3])

---

### Synth state

```text
DTCM / AXI SRAM
```

в зависимости от размера.

---

### Большие sample buffers

```text
AXI SRAM
0x24000000
```

Но здесь уже нужно учитывать D-cache.

ST отдельно указывает, что для DMA/shared buffers необходимо либо использовать подходящую non-TCM память с cache maintenance, либо настроить MPU/cache attributes; cache line — 32 bytes. ([GitHub][7])

---

# 26. D-Cache для PurrMidi

Я предлагаю **не отключать D-Cache глобально**.

Правильная архитектура:

```text
CPU
 │
 ├── D-Cache enabled
 │
 ├── DTCM
 │    └── MIDI queue / realtime synth state
 │
 ├── SRAM1/2
 │    └── non-cacheable audio DMA buffers
 │
 ├── SRAM3
 │    └── USB buffers
 │
 └── AXI SRAM
      └── general data / samples
```

Это даст H743 нормальную производительность и не создаст типичную проблему:

```text
CPU sees new audio data
DMA sees old cached audio data
```

---

# 27. Почему именно эта архитектура хорошо подходит PurrMidi

Главное преимущество H743 здесь не только 480 MHz.

Мы получаем возможность развести realtime ресурсы:

```text
DTCM
    MIDI queue
    synth voice state
    hot DSP state

D2 SRAM
    audio DMA
    USB buffers

AXI SRAM
    samples
    general heap
    larger buffers

QSPI
    persistent/sample storage

SD
    filesystem/sample library
```

То есть можно избежать ситуации F411, когда USB processing, synth, audio DMA и остальные задачи конкурируют практически за один маленький набор ресурсов.

---

# 28. Что я считаю окончательным PurrMidi pinout

### Audio

```text
PF6  → PCM5102A DIN
PF8  → PCM5102A BCLK
PF9  → PCM5102A LRCK
```

### USB MIDI

```text
PA11 → USB D-
PA12 → USB D+
PA9  → reserved VBUS
PA10 → reserved USB/ID
```

### TFT

```text
PE12 → SCK
PE14 → MOSI
PE11 → CS
PE13 → DC
PE10 → BL
```

### SD

```text
PC8  → D0
PC9  → D1
PC10 → D2
PC11 → D3
PC12 → CLK
PD2  → CMD
PD4  → DETECT
```

### QSPI

```text
PB2  → CLK
PB6  → CS
PD11 → IO0
PD12 → IO1
PE2  → IO2
PD13 → IO3
```

### SPI Flash

```text
PB3 → SCK
PB4 → MISO
PD7 → MOSI
PD6 → CS
```

### Diagnostics

```text
PB10 → USART3_TX
PB11 → USART3_RX
```

### Debug

```text
PA13 → SWDIO
PA14 → SWCLK
NRST → RESET
```

---

# 29. Что вывести на гребёнки

Поскольку у WeAct уже есть длинные 2.54-mm header rows, я бы **не пытался перепаивать отдельные GPIO или делать нестандартную разводку**.

Для PurrMidi при пайке гребёнок особенно важны:

### Обязательно вывести

```text
GND
3V3
5V/VBUS

PB10
PB11

PF6
PF8
PF9
```

Это даст:

```text
UART
PCM5102A
```

на удобном разъёме.

### Желательно оставить доступными

```text
PA0
PA1
PA2
PA3
PB0
PB1
PC4
PC5
PE7
PE8
PE9
PE15
PF0
PF1
PF2
PF3
PF4
PF5
PF10
```

Это наш резерв для:

```text
buttons
encoder
LEDs
MIDI control
volume encoder
menu controls
```

---

## Финальный статус

**PurrMidi H743 Rev.1**:

| Ресурс       | Назначение                  |
| ------------ | --------------------------- |
| USB OTG FS   | MIDI Host                   |
| SAI1 Block B | PCM5102A                    |
| DMA2 Stream4 | Audio TX                    |
| SPI4         | ST7735                      |
| SDMMC1 4-bit | microSD                     |
| QUADSPI      | 8 MB QSPI Flash             |
| SPI1         | 8 MB SPI Flash              |
| USART3       | diagnostics                 |
| SWD          | debug/programming           |
| DTCM         | MIDI queue + realtime state |
| D2 SRAM1/2   | audio DMA                   |
| D2 SRAM3     | USB buffers                 |
| AXI SRAM     | samples/general buffers     |

[1]: https://www.ti.com/product/PCM5102A?utm_source=chatgpt.com "PCM5102A data sheet, product information and support | TI.com"
[2]: https://www.alldatasheet.jp/html-pdf/1179082/stmicroelectronics/stm32h743vi/145785/64/stm32h743vi.html?utm_source=chatgpt.com "stm32h743vi datasheet(64/357 Pages) STMICROELECTRONICS | 32-bit Arm® Cortex®-M7 480MHz MCUs, up to 2MB Flash, up to 1MB RAM, 46 com. and analog interfaces"
[3]: https://www.st.com/content/ccc/resource/technical/document/reference_manual/group0/c9/a3/76/fa/55/46/45/fa/DM00314099/files/DM00314099.pdf/jcr%3Acontent/translations/en.DM00314099.pdf?utm_source=chatgpt.com "STM32H742, STM32H743/753 and STM32H750 Value line advanced Arm®-based 32-bit MCUs - Reference manual"
[4]: https://github.com/vznncv/TARGET_WEACT_H743VI?utm_source=chatgpt.com "GitHub - vznncv/TARGET_WEACT_H743VI: Mbed OS 6 port of WeAct MiniSTM32H7xx board · GitHub"
[5]: https://doc.platan.ru/pdf/datasheets/stm/STM32H743.pdf?utm_source=chatgpt.com "STM32H743VIT6"
[6]: https://github.com/STMicroelectronics/stm32h747i-disco-bsp/blob/main/stm32h747i_discovery_audio.h?utm_source=chatgpt.com "stm32h747i-disco-bsp/stm32h747i_discovery_audio.h at main · STMicroelectronics/stm32h747i-disco-bsp · GitHub"
[7]: https://github.com/STMicroelectronics/STM32CubeH7/blob/master/Projects/STM32H743I-EVAL/Examples/FMC/FMC_SDRAM_DataMemory/readme.txt?utm_source=chatgpt.com "STM32CubeH7/Projects/STM32H743I-EVAL/Examples/FMC/FMC_SDRAM_DataMemory/readme.txt at master · STMicroelectronics/STM32CubeH7 · GitHub"

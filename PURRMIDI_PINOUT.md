# PurrMidi — Initial Pinout

Target: **WeAct STM32H743VIT6 Mini**

Only connections required for the initial bring-up are listed here. SD, Flash, TFT, camera and other unused peripherals are deliberately omitted.

## USB MIDI Host

| STM32H743 | Function | Connection |
|---|---|---|
| PA11 | USB_OTG_FS_DM | USB D− |
| PA12 | USB_OTG_FS_DP | USB D+ |
| PA10 | USB ID | reserved |
| PA9 | USB/VBUS-related | reserved |

### VBUS

The MIDI keyboard needs **5 V USB VBUS** supplied by the host.

Before connecting it, verify on the physical board:
- USB-C VBUS routing;
- availability of 5 V when externally powered;
- ability to source 5 V to the host device;
- current limiting/load switch, if present.

Do not assume host VBUS is available until measured.

## PCM5102A

Use **SAI1 Block B**.

| STM32H743 | Function | PCM5102A |
|---|---|---|
| PF8 | SAI1_SCK_B | BCK |
| PF9 | SAI1_FS_B | LRCK / WS |
| PF6 | SAI1_SD_B | DIN |
| PF7 | SAI1_MCLK_B | not connected |

Power:
- PCM5102A GND → GND
- PCM5102A VCC → appropriate supply
- OUTL/OUTR → audio output

Initial format:
- I2S / Philips
- 48 kHz
- stereo
- 32-bit container
- MCLK unused

## Diagnostic UART

USART3:

| STM32H743 | Function | USB-UART |
|---|---|---|
| PB10 | USART3_TX | RX |
| PB11 | USART3_RX | TX |
| GND | GND | GND |

Format: `115200 8N1`

Used for startup, USB, MIDI and synthesizer diagnostics.

## Audio DMA

| Resource | Assignment |
|---|---|
| Controller | DMA2 |
| Stream | Stream4 |
| Request | SAI1_B |
| Direction | Memory → Peripheral |
| Mode | Circular |

DMA2 Stream4 is reserved for audio.

## SWD

| Pin | Function |
|---|---|
| PA13 | SWDIO |
| PA14 | SWCLK |
| NRST | Reset |

Keep SWD available.

## Initial summary

### Connected now

```text
USB:
PA11  USB D−
PA12  USB D+

Audio:
PF6   PCM5102A DIN
PF8   PCM5102A BCK
PF9   PCM5102A LRCK

UART:
PB10  USART3_TX
PB11  USART3_RX
```

### Reserved

```text
PA9   USB/VBUS/boot-related
PA10  USB ID / boot-related
PA13  SWDIO
PA14  SWCLK
PF7   SAI1 MCLK (unused)
DMA2 Stream4  audio
```

## Not used initially

- microSD / SDMMC
- onboard ST7735 TFT
- 8 MB QSPI Flash
- 8 MB SPI Flash
- DCMI camera
- additional buttons
- rotary encoder
- additional LEDs
- other control interfaces

These are added only after the USB MIDI → synthesizer → PCM5102A path is stable.

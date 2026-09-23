# PurrMidi — Architecture

## Target hardware

**WeAct STM32H743VIT6 Mini**
- STM32H743VIT6, Cortex-M7
- CPU up to 480 MHz
- 2 MB Flash, 1 MB RAM
- HSE 25 MHz, LSE 32.768 kHz

**Audio DAC:** PCM5102A

```text
USB MIDI keyboard
        │
        ▼
STM32H743 USB Host
        │
        ▼
MIDI parser / event queue
        │
        ▼
Software synthesizer
        │
        ▼
Audio buffer + DMA
        │
        ▼
SAI1 Block B
        │
        ▼
PCM5102A
        │
        ▼
Audio output
```

## Initial scope

First milestone uses only:
- USB MIDI Host
- MIDI event reception and diagnostics
- software synthesizer
- SAI audio output
- PCM5102A
- USART3 diagnostic console

TFT, microSD, SPI/QSPI Flash, camera and additional controls are intentionally excluded from the initial configuration.

## USB MIDI Host

- PA11 — USB_OTG_FS_DM
- PA12 — USB_OTG_FS_DP
- PA10 — USB ID / reserved
- PA9 — USB/VBUS/boot-related / reserved

USB Host requires suitable **5 V VBUS power** for the connected MIDI keyboard. The exact VBUS power path on the physical WeAct board must be verified before relying on it.

## Diagnostic UART

USART3:
- PB10 — TX
- PB11 — RX
- 115200 8N1

USART3 is used so PA9/PA10 remain reserved for USB/boot-related functions.

## Audio

Use **SAI1 Block B**:

- PF6 — SAI1_SD_B → PCM5102A DIN
- PF8 — SAI1_SCK_B → PCM5102A BCK
- PF9 — SAI1_FS_B → PCM5102A LRCK/WS
- PF7 — MCLK unused

Initial format:
- I2S / Philips
- stereo
- 48 kHz
- 32-bit container
- TX only
- MCLK disabled

## Audio DMA

- DMA2 Stream4
- request: SAI1_B
- circular mode

DMA buffers belong in D2 SRAM1/SRAM2, not DTCM.

Recommended:
- D2 SRAM1: `0x30000000`
- D2 SRAM2: `0x30020000`

Keep buffers aligned to the 32-byte Cortex-M7 cache line.

DMA2 Stream4 is reserved for audio.

## H743 memory policy

| Memory | Address | Initial use |
|---|---:|---|
| DTCM RAM | `0x20000000` | CPU-only MIDI/event state |
| AXI SRAM | `0x24000000` | general/sample data |
| D2 SRAM1 | `0x30000000` | audio DMA |
| D2 SRAM2 | `0x30020000` | audio DMA |
| D2 SRAM3 | `0x30040000` | USB/peripheral buffers |
| D3 SRAM4 | `0x38000000` | later use |

DMA1/DMA2 cannot directly access DTCM.

Keep D-cache enabled. DMA memory should use either MPU non-cacheable regions or explicit cache maintenance. For dedicated audio buffers, a non-cacheable MPU region is preferable.

## Clock

Target:
- HSE 25 MHz
- CPU 480 MHz
- USB clock 48 MHz
- stable SAI clock for 48 kHz audio

Exact PLL configuration is part of H743 bring-up.

## Reserved resources

- PA9/PA10 — USB/boot-related
- PA11/PA12 — USB
- PA13/PA14 — SWD
- PF6/PF8/PF9 — audio
- PB10/PB11 — diagnostic UART
- DMA2 Stream4 — audio

Keep SWD available.

## Development order

1. MCU starts with intended clock.
2. USART3 works.
3. SAI1 Block B generates test audio.
4. PCM5102A produces sound.
5. USB Host detects MIDI keyboard.
6. MIDI packets are parsed and reported.
7. MIDI events enter a reliable queue.
8. Synthesizer generates audio.
9. Audio DMA remains stable under MIDI load.

Only then add other board peripherals.

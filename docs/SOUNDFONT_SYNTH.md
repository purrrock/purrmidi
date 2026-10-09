# SoundFont Synthesizer (TinySoundFont) in PurrMidi

## 1. Overview
PurrMidi includes a SoundFont2 (SF2) synthesizer engine powered by an adapted version of [TinySoundFont](https://github.com/schellingb/TinySoundFont). The engine reads instrument sample data directly from the microSD card without loading the entire SF2 bank into RAM.

## 2. File Location & SD Card Requirement
* **Path**: `0:/SNDFNT.SF2` in the root directory of the FATFS microSD card.
* **Requirements**: FAT32 formatted microSD card mounted via SDMMC1 (4-bit bus).
* **Missing or Corrupted File**: If `0:/SNDFNT.SF2` is missing or invalid, the SoundFont engine produces silence safely without blocking or affecting other synthesizer engines.

## 3. Instrument Selection
Synthesizers are selected runtime via MIDI Program Change messages:

| Program Change | Engine | Description |
|---|---|---|
| 0 | E-Piano | Polyphonic FM Electric Piano (default) |
| 1 | Pluck | Karplus-Strong Physical Model |
| 2 | Sine | Single Sine Wave Diagnostic Engine |
| 3 | Organ | Hammond Tonewheel Organ (bfreeOrgan2) |
| 4 | SoundFont | SF2 SoundFont Synthesizer (TinySoundFont) |

Program numbers higher than 4 wrap modulo 5 (`program % SYNTH_ENGINE_COUNT`).

## 4. Streaming Architecture & RAM Cache
To avoid loading multi-megabyte SF2 files into STM32 RAM:
* **Metadata in RAM**: SF2 headers, presets, instruments, generators, and region parameters are parsed at startup and stored in RAM.
* **PCM Sample Streaming**: The raw 16-bit PCM sample data in the `smpl` chunk remains on the SD card.
* **Fixed-size Sample Cache**:
  * Total size: **64 KB** (32 blocks x 1024 16-bit mono PCM samples = 2048 bytes per block).
  * Lock-free atomic state management (`EMPTY`, `LOADING`, `READY`) between main loop and audio DMA interrupt.
  * LRU eviction policy for replacing cached sample blocks.
* **Non-Blocking Audio Thread**:
  * Audio DMA callbacks query the cache for requested sample frames.
  * On a cache miss, a load request is pushed to a Single-Producer Single-Consumer (SPSC) queue.
  * Missing samples return silence (`0`) to prevent audio interrupt stalls or FATFS file I/O in the interrupt context.
* **Main Loop Service**:
  * `SoundFontSynth_Process()` runs inside `main.c`'s `while(1)` loop alongside USB-MIDI processing.
  * Pops requests from the SPSC queue, reads PCM blocks via FATFS (`f_read`), and populates cache slots.

## 5. Hardware Verification
1. Place a valid SF2 bank file named `SNDFNT.SF2` in the root of a microSD card.
2. Insert the card into the microSD slot on the STM32H743 board.
3. Power on the device.
4. Send a MIDI Program Change `4` (or `9`, `14`, etc.) from a MIDI controller or sequencer.
5. Play notes on the keyboard and verify polyphonic playback.

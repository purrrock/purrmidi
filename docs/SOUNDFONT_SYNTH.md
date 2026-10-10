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
  * Firmware: **64 KB** (32 blocks x 1024 16-bit mono PCM samples = 2048 bytes per block). Host (Windows/Linux) build: 512 KB (256 blocks), because a looped piano note keeps ~14 blocks of loop resident per sample (L and R) and 32 blocks are used up by a couple of notes. Override with `-DSF2_CACHE_BLOCKS=N`.
  * Lock-free atomic state management (`EMPTY`, `LOADING`, `READY`) between main loop and audio DMA interrupt.
  * LRU eviction policy for replacing cached sample blocks.
* **Non-Blocking Audio Thread**:
  * Audio DMA callbacks query the cache for requested sample frames and never touch the file.
  * A sample that is not in the cache is played as silence (`0`) and a load request is pushed to a Single-Producer Single-Consumer (SPSC) queue. This is the *last resort*: it is audible as a click / dropout, see below.
* **Main Loop Service** (all file I/O lives here):
  * `SoundFontSynth_Process()` runs inside `main.c`'s `while(1)` loop alongside USB-MIDI processing (on the Windows host: in the main loop of `purrmidi_win`).
  * It first services the blocks the audio thread missed, then **reads ahead**: every block the audio thread reads is flagged, and the following `SF2_READAHEAD_BLOCKS` (3) blocks are loaded before the voice reaches them.
  * One call performs at most `SF2_MAX_BLOCKS_PER_PROCESS` file reads (firmware 2, host 64). A block that fails to load is retried on the next call.
* **Note start**: `SoundFontSynth_NoteOn()` looks up the regions the note will play (`tsf_channel_get_note_start_samples()`) and synchronously loads the first `SF2_PRELOAD_BLOCKS_PER_VOICE` (2) blocks of each of them *before* the note is queued, so the attack is never replaced by silence.

### Why it must read ahead
A purely reactive cache (load a block only after the audio thread missed it) cannot work: while the block is being loaded the voice keeps advancing, so every block boundary (every ~23 ms of a note) becomes a hole of silence (clicks / noise). If the main loop is slower than the voices consume data (e.g. `sleep_for(1ms)` on Windows really sleeps ~15.6 ms unless `timeBeginPeriod(1)` is used), the sound disappears completely after the first few milliseconds of the note.

### Threading of MIDI events
TinySoundFont is not thread safe (`tsf_note_on()` publishes a voice before its envelopes and pan are initialised). `NoteOn` / `NoteOff` / `ControlChange` / `ProgramChange` are therefore queued in a lock-free SPSC FIFO (producer: main, consumer: audio) and applied at the start of the next `SoundFontSynth_FillStereoBuffer()`, like in the other engines.

### Diagnostics
`SF2Cache_GetStats()` returns `miss_samples` (samples replaced by silence), `blocks_loaded` and `load_errors`. `purrmidi_win --play --verbose` prints `[SF2] cache underrun: ...` whenever `miss_samples` grows and a summary on exit. On a healthy system `miss_samples` stays 0.

## 5. Hardware Verification
1. Place a valid SF2 bank file named `SNDFNT.SF2` in the root of a microSD card.
2. Insert the card into the microSD slot on the STM32H743 board.
3. Power on the device.
4. Send a MIDI Program Change `4` (or `9`, `14`, etc.) from a MIDI controller or sequencer.
5. Play notes on the keyboard and verify polyphonic playback.

## STM32: load at boot, memory and "next instrument"

* The SoundFont is loaded **once at power-up** (`SF2_LOAD_AT_BOOT` in `Core/Src/main.c`); progress is printed to the UART
  (`[SF2] Loading SoundFont from SD...` / `[SF2] SoundFont ready` / `[SF2] SoundFont NOT available ...`).
  Loading blocks for a while (SD init + parsing the preset tables), so it must not happen while playing.
  Hot-plugging the card is not supported: reboot after inserting it.
* If the load fails, the engine is reported as unavailable (`SynthEngine_IsAvailable()`), and the
  Program Change 127 ("next instrument") cycle skips it: `... -> organ -> epiano`.
* TinySoundFont needs roughly 170 KB of heap for the preset tables of a ~27 MB bank. DTCM has only ~20 KB spare,
  so the newlib heap lives in the `.axi_heap` section of AXI SRAM (`_Axi_Heap_Size` in `STM32H743xx_FLASH.ld`, 320 KB).
* Program Change 127 repeats closer than `PC_NEXT_REPEAT_GUARD_MS` (300 ms) are ignored: some keyboards send the
  message twice per button press (press + release).

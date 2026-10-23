<p align="center">
  <img src="./logo/PurrMIDI_logo.png" alt="PurrMIDI logo" width="200"/>
</p>

# PurrMidi

[Русская версия: README_RU.md](README_RU.md)

> **Note:** the rest of the documentation (`docs/`, `PINOUT.md`, `TODO.md`, etc.) and the code comments are currently written in Russian only.

**PurrMidi** is a standalone MIDI synthesizer based on the STM32: connect a USB MIDI keyboard, and the microcontroller generates the sound and outputs it through an external audio DAC.

The project includes STM32H743 firmware, a Windows application for testing the sound engines on a PC (it can run without a MIDI keyboard), and six sound engines (electric piano, nylon string, lead, organ, violin, SoundFont).
The project is under active development and is not yet a finished production product.

## Hardware

* **Microcontroller:** WeAct STM32H743VIT6 Mini (Cortex-M7).
* **Audio DAC:** PCM5102A.
* **MIDI input:** USB MIDI keyboard via USB OTG.
* **Control button:** the User Key K1 on the microcontroller board, used to switch instruments.
* **Display:** onboard 160×80 ST7735 TFT showing MIDI status, active notes, the chord name and the selected instrument.
* **Memory card:** microSD (FAT32). It is used **only at startup**, to automatically write/update the SoundFont bank `0:/SNDFNT.SF2` into the board's Flash. The SD card is not needed or used during play.

Physical connections and pinout are described in `PINOUT.md`.

## Basic operation

**Connecting.** Plug the USB MIDI keyboard into the board's USB OTG connector (the device works as a USB Host) and connect the PCM5102A audio output to an amplifier or headphones. Power it on: the E-Piano starts.
If the board is powered through the same USB connector that the MIDI keyboard uses (yes, this is possible!), you will need a Y-splitter cable.

**Selecting an instrument.** Use a Program Change message from the MIDI keyboard or the **K1** button on the board.

| Program Change | Instrument | Description |
| -------------: | ---------- | ----------- |
| `0` | **E-Piano** | FM electric piano (default) |
| `1` | **Pluck** | Nylon guitar string |
| `2` | **Lead** | Analog lead |
| `3` | **Organ** | Tonewheel organ |
| `4` | **Violin** | Violin |
| `5…` | **SoundFont** | Presets of the SoundFont2 bank: `5` is the first, `6` the second, and so on |
| `127` | — | Next instrument in a cycle (K1 works the same way) |

The instrument name and active notes are shown on the display. The selection is not saved: after power-up the device always starts with the E-Piano.

**Keyboard controls.**

| Function | MIDI |
| -------- | ---- |
| Master volume | CC7 (works on all instruments) |
| Sustain pedal | CC64 |
| Pluck: brightness / decay | CC71, CC74 / CC72; the mod wheel (CC1) makes the sound brighter, and a released wheel does not change the timbre |
| Organ: drawbars, chorus | CC20–28 (drawbars), CC70 (chorus on/off), CC71/72 (chorus speed and depth) |
| Violin: vibrato, bow | CC1 vibrato, CC70/71/75 bow position, pressure and noise, Pitch Bend, aftertouch |
| E-Piano, Lead | see [docs/EPIANO_SYNTH.md](docs/EPIANO_SYNTH.md), [docs/ENGINES.md](docs/ENGINES.md) (in Russian) |

**Custom SoundFont.** Put the bank file in the root of the microSD card named `SNDFNT.SF2`, insert the card and power the device on: the bank is automatically written to Flash memory (30–60 seconds, progress is shown on the display). After that the card is no longer needed. Details and troubleshooting are in [docs/SD_CARD_AND_QSPI.md](docs/SD_CARD_AND_QSPI.md) (in Russian).
`SNDFNT.SF2` must be smaller than 8 MB, otherwise it will not fit into Flash. You can try to reduce the file size with `/SoundFonts/sf2_shrink.py` or with the free Polyphone utility.
The `/SoundFonts` directory contains several small free instrument banks.

## Building

### Getting the sources

```bash
git clone https://github.com/purrrock/purrmidi
```

Everything needed for the build is in the repository itself (third-party code is in `Middlewares/Third_Party/` and `Platform/display/st7735/`).

### STM32 firmware

```bash
cmake --preset Debug
cmake --build --preset Debug
```

For a release build:

```bash
cmake --preset Release
cmake --build --preset Release
```

### Windows version

On Windows with CMake and the MSVC compiler installed:

```bash
cmake --preset win-msvc-release
cmake --build --preset win-msvc-release
```

Instructions and additional details: `docs/WINDOWS_BUILD.md` (in Russian).

The repository structure and the list of per-subsystem documents are at the end of [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (in Russian).

## Licenses

* **Third-party components.** The project uses HAL/CMSIS, the USB Host Library (ST), FatFs, TinySoundFont, parts of DaisySP (`Oscillator`, `Svf`, `Adsr`), bfreeOrgan2 and others. The full list, licenses and terms are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
* **PurrMidi's own code is licensed under MIT** (see [`LICENSE`](LICENSE)).

## Repository

Source code: https://github.com/purrrock/purrmidi

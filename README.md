# PurrMidi

**PurrMidi** — экспериментальный автономный MIDI-синтезатор на базе STM32.

Цель проекта — создать компактный аппаратный MIDI sound module: подключить USB MIDI-клавиатуру, генерировать звук на микроконтроллере и выводить его через внешний аудио ЦАП.

Проект развивается как практический embedded-аудио проект с постепенным переходом от простого прототипа к полноценному полифоническому синтезатору.

## Hardware

### Current prototype

* **MCU:** STM32F411CEU6
* **Audio DAC:** PCM5102A

  * I2S audio interface
  * stereo output
* **MIDI input:** USB MIDI keyboard
* **Controls:** push buttons
* **Indicators:** LEDs
* Breadboard and jumper wires
* Headphones / powered speakers

### Planned MCU

В дальнейшем основной контроллер планируется заменить на:

* **NUCLEO-F446RE / STM32F446RE**

STM32F446 имеет более подходящие для аудио DSP ресурсы и станет основной платформой для последующих этапов проекта.

## Concept

Основной тракт:

```text
USB MIDI Keyboard
       │
       ▼
   MIDI input
       │
       ▼
 MIDI event parser
       │
       ▼
  Synthesizer
       │
       ▼
 Audio sample buffer
       │
       ▼
      I2S
       │
       ▼
    PCM5102A
       │
       ▼
 Headphones / Speakers
```

Кнопки и светодиоды используются для управления и индикации состояния синтезатора.

## Goals

Проект развивается поэтапно.

### Phase 1 — Hardware bring-up

* [X] STM32F411 clock and basic firmware
* [ ] Debug UART
* [ ] USB MIDI input
* [ ] MIDI message parser
* [X] PCM5102A connection
* [X] I2S output
* [X] Generate a test tone
* [X] Play the test tone through PCM5102A

### Phase 2 — Basic synthesizer

* [ ] MIDI Note On / Note Off
* [ ] Single oscillator
* [ ] Multiple simultaneous voices
* [ ] ADSR envelope
* [ ] Basic waveform selection
* [ ] Master volume
* [ ] Audio buffer / DMA based playback

### Phase 3 — Playable instrument

* [ ] Polyphony
* [ ] Velocity handling
* [ ] Pitch bend
* [ ] Sustain pedal
* [ ] MIDI Control Change
* [ ] Presets
* [ ] Button controls
* [ ] LED status indication

### Phase 4 — Sound engine

Possible synthesis techniques:

* subtractive synthesis
* wavetable synthesis
* sample playback
* digital filters
* LFO
* modulation
* effects

The exact architecture will be determined by the capabilities of the target MCU and the requirements of the sound engine.

## Software Architecture

The firmware should keep hardware-specific code separate from the synthesizer itself.

A possible architecture:

```text
USB MIDI
   │
   ▼
 MIDI driver
   │
   ▼
 MIDI parser
   │
   ▼
 MIDI events
   │
   ▼
 Synth engine
   │
   ▼
 Audio mixer
   │
   ▼
 Audio output
   │
   ▼
 I2S + DMA
```

The synthesizer engine should not depend directly on STM32 peripheral APIs.

This should make it possible to develop and test the audio engine independently from the final MCU and hardware configuration.

## Audio

The PCM5102A is used as the external audio DAC.

The MCU generates digital PCM samples and transfers them to the DAC using I2S.

The intended audio path is:

```text
Synthesizer
    │
    │ PCM samples
    ▼
Audio buffer
    │
    │ DMA
    ▼
I2S peripheral
    │
    ▼
PCM5102A
    │
    ▼
Analog audio
```

DMA should be used for continuous audio streaming so that the CPU is not required to manually transmit every sample.

## Development

The initial development platform is the STM32F411CEU6 on a breadboard.

Development will proceed incrementally: each hardware block is first brought up and tested independently before integrating it into the synthesizer.

The project is intended to remain small enough to understand and modify at the firmware level rather than relying on a large external synthesizer framework.

## Repository

Source code:

[github.com/purrrock/purrmidi](https://github.com/purrrock/purrmidi?utm_source=chatgpt.com)

## Status

**Early development.**

The current priority is hardware bring-up and establishing a minimal end-to-end path:

```text
MIDI keyboard → STM32 → synthesized tone → I2S → PCM5102A → audio output
```

Once this path works reliably, the synthesizer engine can be developed incrementally.

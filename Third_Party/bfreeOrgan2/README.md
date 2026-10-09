# Vendored bfreeOrgan2 Organ Synthesizer Engine

This directory contains vendored source files from the `bfreeOrgan2` Tonewheel Organ DSP implementation and its `flexYnth` synthesis framework, adapted for PurrMidi.

## Upstream Provenance

* **bfreeOrgan2:** https://github.com/fcaspe/bfreeOrgan2
* **flexynth:** https://github.com/fcaspe/flexynth
* **Author:** Franco Caspe
* **License:** Apache License 2.0

## Imported Files

* `flexynth_base.hpp`
* `synth_organ.hpp`
* `synth_organ.cpp`
* `components/voice_base.hpp`
* `components/voice_organ.hpp`
* `components/voice_organ.cpp`
* `components/tables.hpp`
* `components/efx_base.hpp`
* `components/efx_chorus.hpp`
* `components/efx_chorus.cpp`
* `components/env_base.hpp`
* `components/env_ar.hpp`
* `components/env_ar.cpp`
* `components/env_click.hpp`
* `components/env_click.cpp`
* `components/midi_base.hpp`
* `components/cc_base.hpp`
* `components/pb_base.hpp`

## Local Modifications

1. **Portability:** Removed `__attribute__((section(".ccmram")))` in `tables.hpp` to allow compilation with MSVC and on STM32H7 (which does not use CCMRAM).
2. **Omni Channel Support:** Removed the MIDI channel 1 constraint (`cmd.channel != 0`) in `synth_organ::push_midi_cmd` to align with PurrMidi's omni MIDI dispatch behavior.
3. **Safety / Bounds Checking:** Added note range validation (`cmd.data >= 24 && cmd.data <= 84`) in `push_midi_cmd` to prevent array out-of-bounds indexing in the 61-key tonewheel switchbox.

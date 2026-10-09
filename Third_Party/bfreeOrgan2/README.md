# Vendored bfreeOrgan2 Organ Synthesizer Engine

This directory contains vendored source files from the `bfreeOrgan2` Tonewheel Organ DSP implementation and its supporting `flexYnth` synthesis framework, adapted for PurrMidi.

## Upstream Provenance & Commit SHAs

* **bfreeOrgan2:** https://github.com/fcaspe/bfreeOrgan2
  * **Commit SHA:** `79e2ad941b58985e66d4c4f0434d938ca6b7742a`
* **flexynth:** https://github.com/fcaspe/flexynth
  * **Commit SHA:** `d3694bff543c0da7125e206200daa81150238d78`
* **Author:** Franco Caspe
* **License:** Apache License 2.0 (see `LICENSE.txt` and `NOTICE`)

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

1. **Portability:** Removed `__attribute__((section(".ccmram")))` in `tables.hpp` to allow compilation with MSVC, GCC, and on STM32H7 (which does not use CCMRAM).
2. **Omni Channel Support:** Removed the MIDI channel 1 constraint (`cmd.channel != 0`) in `synth_organ::push_midi_cmd` to align with PurrMidi's omni MIDI dispatch behavior.
3. **Safety / Bounds Checking:** Added note range validation (`cmd.data >= 24 && cmd.data <= 84`) in `push_midi_cmd` to prevent array out-of-bounds indexing in the 61-key tonewheel switchbox.
4. **Voice Management & Re-triggering:** Fixed voice allocation on `NOTE_ON` to re-use an active voice playing the same pitch rather than spawning duplicate voices, and updated `NOTE_OFF` and CC 120/123 to deactivate all active instances of matching pitches.
5. **Real-time Safety & SPSC Queue:** Integrated `OrganSynth_FillStereoBuffer` with an event limit per block (16 events) and atomic queue overflow recovery (`deactivate_all()`) to guarantee no stuck notes when FIFO overflows occur under heavy load.

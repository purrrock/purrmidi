#!/usr/bin/env bash
# Сборка звуковых движков PurrMIDI в WebAssembly (site/demo/purrmidi.wasm) для веб-демо.
#
# Нужен только Zig (он содержит clang, libc++ и wasi-libc):
#     pip install ziglang        # или https://ziglang.org/download/
# Запуск из любого каталога:  Platform/web/build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-$ROOT/site/demo/purrmidi.wasm}"
ZIG="${ZIG:-python3 -m ziglang}"
DAISY="$ROOT/Middlewares/Third_Party/DaisySP_lite/Source"
ORGAN="$ROOT/Middlewares/Third_Party/bfreeOrgan2"

mkdir -p "$(dirname "$OUT")"

# C-файл компилируется отдельно: опция -std=c++17 для C недопустима.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# shellcheck disable=SC2086
$ZIG cc -target wasm32-wasi -std=c11 -O2 -DNDEBUG -c -I"$ROOT/Engine/midi" \
  "$ROOT/Engine/midi/chord_detector.c" -o "$TMP/chord_detector.o"

# shellcheck disable=SC2086
$ZIG c++ -target wasm32-wasi -mexec-model=reactor -std=c++17 -O2 -fno-exceptions -fno-rtti \
  -DNDEBUG -DPURRMIDI_HOST_BUILD -mbulk-memory -msimd128 \
  -Wl,--strip-all -Wl,--export-memory -Wl,--no-entry -Wl,--initial-memory=33554432 -Wl,--max-memory=268435456 \
  -I"$ROOT/Engine/synth" -I"$ROOT/Engine/midi" -I"$ROOT/Platform/qspi_xip" -I"$ROOT/Platform/web" \
  -isystem "$ROOT/Middlewares/Third_Party/TinySoundFont" \
  -isystem "$DAISY" -isystem "$DAISY/Utility" -isystem "$DAISY/Synthesis" -isystem "$DAISY/Filters" -isystem "$DAISY/Control" \
  -isystem "$ORGAN" -isystem "$ORGAN/components" \
  "$ROOT/Platform/web/purrmidi_web.cpp" \
  "$ROOT/Platform/web/sf2_loader_web.cpp" \
  "$ROOT/Engine/synth/synth_engine.cpp" \
  "$ROOT/Engine/synth/synth_arena.cpp" \
  "$ROOT/Engine/synth/epiano_synth.cpp" \
  "$ROOT/Engine/synth/pluck_synth.cpp" \
  "$ROOT/Engine/synth/lead_synth.cpp" \
  "$ROOT/Engine/synth/organ_synth.cpp" \
  "$ROOT/Engine/synth/violin_synth.cpp" \
  "$ROOT/Engine/synth/soundfont_synth.cpp" \
  "$ROOT/Engine/synth/tinysoundfont_impl.cpp" \
  "$TMP/chord_detector.o" \
  "$ORGAN/synth_organ.cpp" \
  "$ORGAN/components/voice_organ.cpp" "$ORGAN/components/efx_chorus.cpp" \
  "$ORGAN/components/env_ar.cpp" "$ORGAN/components/env_click.cpp" \
  "$DAISY/Synthesis/oscillator.cpp" "$DAISY/Filters/svf.cpp" "$DAISY/Control/adsr.cpp" \
  -o "$OUT"

ls -l "$OUT"

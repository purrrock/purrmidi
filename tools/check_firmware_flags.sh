#!/usr/bin/env bash
# Проверяет по compile_commands.json, что у исходников прошивки правильные флаги компиляции:
#   * горячие аудио-файлы собираются с -O2 (в Release; Debug по умолчанию тоже, см. PURRMIDI_HOT_OPT_IN_DEBUG);
#   * код проекта (Engine/, Platform/, Core/Src/main.c) собирается с -Wextra;
#   * сгенерированные CubeMX и сторонние файлы остаются без -Wextra (там он даёт шум при каждой регенерации).
# Нужен, потому что такие ошибки (например, set_source_files_properties(... COMPILE_OPTIONS ...) молча заменяет -O2)
# не дают ни предупреждений, ни падения тестов: код просто становится медленнее.
#
# Использование: bash tools/check_firmware_flags.sh build/Release/compile_commands.json
set -euo pipefail

cc="${1:?usage: check_firmware_flags.sh <compile_commands.json>}"

python3 - "$cc" <<'PY'
import json, re, sys

entries = {e["file"]: e.get("command") or " ".join(e["arguments"]) for e in json.load(open(sys.argv[1]))}

def find(suffix):
    hits = [c for f, c in entries.items() if f.replace("\\", "/").endswith("/" + suffix)]
    if not hits:
        sys.exit("FAIL: %s не найден в compile_commands.json" % suffix)
    return hits[0]

def has(cmd, flag):
    return re.search(r"(?<![\w-])%s(?![\w-])" % re.escape(flag), cmd) is not None

hot = ["Engine/synth/pluck_synth.cpp", "Engine/synth/epiano_synth.cpp", "Engine/synth/lead_synth.cpp",
       "Engine/synth/organ_synth.cpp", "Engine/synth/violin_synth.cpp", "Engine/synth/soundfont_synth.cpp",
       "Engine/synth/synth_engine.cpp"]
own = hot + ["Core/Src/main.c", "Platform/midi_usb/usbh_midi.c", "Platform/display/display.c",
             "Platform/system/diag.c", "Engine/audio/audio_halves.c", "Engine/midi/midi_queue.c"]
foreign = ["FATFS/Target/user_diskio.c", "USB_HOST/Target/usbh_conf.c", "Middlewares/Third_Party/FatFs/src/ff.c"]

bad = []
for f in hot:
    if not has(find(f), "-O2"):
        bad.append("%s: нет -O2 (горячий аудио-файл)" % f)
for f in own:
    if not has(find(f), "-Wextra"):
        bad.append("%s: нет -Wextra" % f)
for f in foreign:
    if has(find(f), "-Wextra"):
        bad.append("%s: есть -Wextra (сгенерированный/сторонний файл должен оставаться на -Wall)" % f)

if bad:
    print("FAIL: неверные флаги компиляции:")
    for b in bad:
        print("  - " + b)
    sys.exit(1)
print("OK: -O2 у %d горячих файлов, -Wextra у %d файлов проекта, у %d чужих его нет" % (len(hot), len(own), len(foreign)))
PY

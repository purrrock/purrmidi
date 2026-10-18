#!/usr/bin/env bash
# Проверка размера прошивки: печатает размеры секций и завершается с ошибкой, если превышен бюджет.
# Использование: tools/check_firmware_size.sh <purrmidi.elf> [макс. FLASH (text+data), байт] [макс. ОЗУ (data+bss), байт]
#
# Линкер и так не даст выйти за физические регионы памяти; бюджеты здесь нужны, чтобы заметить РОСТ заранее,
# пока запас ещё есть (STM32H743: 2 МБ FLASH; ОЗУ - сумма регионов DTCM/AXI/D2/ITCM).
# Бюджеты по умолчанию - текущий размер Release (500 КБ / 551 КБ) с запасом на разумный рост; поднимайте их осознанно.
set -euo pipefail

elf="${1:?usage: $0 <elf> [max_flash] [max_ram]}"
max_flash="${2:-786432}"      # 768 КиБ
max_ram="${3:-600000}"

size_tool="${SIZE_TOOL:-arm-none-eabi-size}"
read -r text data bss _ <<<"$("$size_tool" -B "$elf" | awk 'NR==2 {print $1, $2, $3}')"

flash=$((text + data))
ram=$((data + bss))
echo "firmware size: text=${text} data=${data} bss=${bss}"
echo "FLASH (text+data) = ${flash} B, budget ${max_flash} B"
echo "RAM   (data+bss)  = ${ram} B, budget ${max_ram} B"

status=0
if (( flash > max_flash )); then echo "ERROR: FLASH budget exceeded by $((flash - max_flash)) B" >&2; status=1; fi
if (( ram > max_ram ));     then echo "ERROR: RAM budget exceeded by $((ram - max_ram)) B" >&2; status=1; fi
exit "$status"

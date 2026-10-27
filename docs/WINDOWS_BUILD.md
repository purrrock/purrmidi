# Windows-приложение `purrmidi_win.exe`

Приложение объединяет MIDI-монитор и все звуковые движки проекта, чтобы отлаживать звук на ПК без прошивки STM32.
Движки те же, что в прошивке (общий `synth_engine.h` и `MIDI_Dispatch`), переключаются сообщением Program Change (см.
[ENGINES.md](ENGINES.md)). MIDI-клавиатура не обязательна: есть виртуальная клавиатура.

## Что понадобится

* **CMake** 3.22+ и **Ninja**.
* Компилятор: MSVC (Visual Studio Build Tools 2022, «Desktop development with C++») или MinGW-w64 (GCC 11+).
* Исходники: `git clone https://github.com/purrrock/purrmidi` (подмодулей нет; RtMidi скачивается CMake при сборке).

## Сборка

```cmd
cmake --preset win-msvc-release
cmake --build --preset win-msvc-release
```

Результат: `build/win-msvc-release/Platform/windows/purrmidi_win.exe`. Для MinGW-w64 используйте пресет `win-mingw-release`
(`build/win-mingw-release/...`), для отладки — `win-msvc-debug`. В VS Code выберите пресет через `CMake: Select Configure Preset`
(для прошивки — `Debug` или `Release`).

## Запуск

| Команда | Что делает |
|---|---|
| `--list` | Список MIDI-портов и аудиоустройств |
| `--monitor --midi-port N` | Печать входящих MIDI-сообщений без звука; `--show-realtime` добавляет Clock (`0xF8`) и Active Sensing (`0xFE`) |
| `--play --midi-port N` | Игра через синтезатор |
| `--wav FILE --wav-seconds S` | Запись выхода в WAV (по умолчанию до 120 с) |
| `--buffer-frames N`, `--gain G` | Размер аудиобуфера (по умолчанию 256) и усиление (1.0) |
| `--octave N`, `--velocity V` | Базовая октава (0–8, по умолчанию 4) и velocity виртуальной клавиатуры (1–127, по умолчанию 100) |
| `--verbose`, `--help` | Подробная диагностика, справка |

Пример: `purrmidi_win.exe --play --midi-port 0 --wav output.wav`. Остановка — `Ctrl+C`. В режиме `--play` печатается
`Synth engine: ...` и при каждой смене `[PC] program N -> synth engine: ...`.

## Виртуальная клавиатура

Включена по умолчанию в режимах `--play` и `--monitor`. Используются физические сканкоды, поэтому раскладка не зависит от языка
ввода (QWERTY / ЙЦУКЕН).

```text
 чёрные:   S  D     G  H  J     L  ;
           C# D#    F# G# A#    C# D#   (следующая октава у L и ;)
 белые:  Z  X  C  V  B  N  M  ,  .  /
         C  D  E  F  G  A  B  C  D  E
```

При базовой октаве 4 клавиша `Z` — C4 (MIDI 60). Управление: `[` и `]` — октава −1 / +1, `TAB` — следующий инструмент
(Program Change `127`).

Многоголосие поддерживается; автоповтор клавиш ОС игнорируется; Note Off уходит для той ноты, что была нажата (даже если октаву
сменили во время удержания); при потере фокуса окна отправляется Note Off для всех удерживаемых нот.

## Особенности

* **Задержка** зависит от `--buffer-frames` и настроек ОС (WASAPI/miniaudio), на STM32 вывод идёт напрямую в SAI DMA.
* **Названия CC в мониторе** (`Damp`, `Decay`, `Sustain`) соответствуют движку Pluck; смысл CC других движков — в
  [ENGINES.md](ENGINES.md#управление-с-клавиатуры).

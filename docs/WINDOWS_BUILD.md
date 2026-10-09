# Сборка и запуск PurrMidi на Windows (Host App)

Данный документ описывает процесс сборки и запуска хостового приложения `purrmidi_win.exe` под Windows. Приложение объединяет MIDI-монитор и один из синтезаторов проекта (`sine`, `pluck` на основе DaisySP Pluck или FM-электропиано `epiano`; выбор — на этапе сборки), позволяя отлаживать и настраивать звучание на ПК без прошивки микроконтроллера STM32.

---

## 1. Необходимое программное обеспечение

Для сборки хостового приложения на ПК понадобятся:
* **VS Code** с расширениями:
  * `ms-vscode.cmake-tools` (CMake Tools)
  * `ms-vscode.cpptools` (C/C++)
* **Инструменты компиляции (один из вариантов):**
  * **MSVC**: Visual Studio Build Tools 2022 (компонент «Разработка классических приложений на C++» / «Desktop development with C++»).
  * **MinGW-w64**: MSYS2 или готовый тулчейн MinGW-w64 (GCC 11+).
* **CMake** версии 3.22 или выше.
* **Ninja** (системы сборки).

---

## 2. Подготовка подмодулей Git

Перед сборкой убедитесь, что все подмодули Git (включая `DaisySP` и вложенный `DaisySP-LGPL`) загружены:

```bash
git submodule update --init --recursive
```

---

## 3. Сборка из командной строки (Presets)

Проект использует `CMakePresets.json`.

### Вариант А: Сборка с MSVC (Visual Studio Build Tools)

```cmd
cmake --preset win-msvc-release
cmake --build --preset win-msvc-release
```

Исполняемый файл будет сохранен в: `build/win-msvc-release/platform/windows/purrmidi_win.exe`.

### Вариант Б: Кросс-компиляция или сборка с MinGW-w64

```bash
cmake --preset win-mingw-release
cmake --build --preset win-mingw-release
```

Исполняемый файл будет сохранен в: `build/win-mingw-release/platform/windows/purrmidi_win.exe`.

### Выбор синтезатора

Синтезатор в `purrmidi_win.exe` выбирается при сборке параметром `PURRMIDI_SYNTH` (`sine`, `pluck` — по умолчанию, `epiano`):

```cmd
:: готовые пресеты
cmake --preset win-msvc-release-epiano
cmake --build --preset win-msvc-release-epiano

:: или для любого пресета
cmake --preset win-msvc-release -DPURRMIDI_SYNTH=sine
```

Доступные пресеты: `win-msvc-release-epiano`, `win-msvc-release-sine`, `win-mingw-release-epiano`, `win-mingw-release-sine`. При запуске в режиме `--play` приложение печатает строку `Synth engine: ...`. Подмодуль DaisySP нужен только для `pluck` и для хостовых тестов (`-DPURRMIDI_BUILD_TESTS=OFF` позволяет собрать `epiano`/`sine` без него).

### Прогон тестов (Host Tests)

```bash
cmake --preset host-tests
cmake --build --preset host-tests
ctest --test-dir build/host-tests --output-on-failure
```

Тесты каждого синтезатора (`test_sine_host`, `test_pluck_host`, `test_epiano_host`) выполняются всегда; `test_queue_dispatch` проверяет цепочку «очередь → диспетчер → выбранный синтезатор», поэтому имеет смысл прогнать набор для каждого значения `-DPURRMIDI_SYNTH=...` (так делает CI).

---

## 4. Сборка в VS Code

1. Скопируйте шаблоны настроек из `docs/vscode/` в папку `.vscode/`:
   ```cmd
   mkdir .vscode
   copy docs\vscode\* .vscode\
   ```
2. Откройте папку проекта в VS Code.
3. Нажмите на панель выбора конфигурации CMake (в строке состояния снизу или через `Ctrl+Shift+P` -> `CMake: Select Configure Preset`).
4. Для хостовой сборки выберите пресет **`win-msvc-release`** (или `win-msvc-debug` / `win-mingw-release`).
5. Для сборки прошивки STM32 выберите пресет **`Release`** или **`Debug`**.
6. Нажмите `F7` или кнопку **Build** в строке состояния.

---

## 5. Использование `purrmidi_win.exe`

### 1) Просмотр доступных устройств (`--list`)

```cmd
purrmidi_win.exe --list
```

Пример вывода:
```text
--- MIDI Input Ports ---
  [0] USB MIDI Keyboard
--- Audio Output Devices ---
  [0] Speakers (Realtek High Definition Audio) (Default)
```

### 2) Запуск MIDI-монитора (`--monitor`)

Печать всех поступающих MIDI-сообщений без вывода звука:

```cmd
purrmidi_win.exe --monitor --midi-port 0
```

Дополнительно отображать служебные Realtime-сообщения (Clock `0xF8`, ActiveSensing `0xFE`):

```cmd
purrmidi_win.exe --monitor --midi-port 0 --show-realtime
```

### 3) Запуск синтезатора (`--play`)

Игра на клавиатуре через синтезатор, выбранный при сборке:

```cmd
purrmidi_win.exe --play --midi-port 0
```

Запись выходного аудиопотока в WAV-файл:

```cmd
purrmidi_win.exe --play --midi-port 0 --wav output.wav --wav-seconds 120
```

Настройка размера аудиобуфера и гейна:

```cmd
purrmidi_win.exe --play --midi-port 0 --buffer-frames 128 --gain 0.9
```

Остановка работы осуществляется нажатием `ENTER` или комбинацией `Ctrl+C`.

---

## 6. Известные ограничения и особенности

1. **Задержка аудио (Latency):** Аудиовывод под Windows осуществляется через подсистему WASAPI/miniaudio. Задержка зависит от размера буфера (`--buffer-frames`) и аудионастроек ОС, в то время как на STM32 вывод осуществляется напрямую в аппаратный SAI DMA.
2. **Полифония:** `pluck` и `sine` — одноголосные; `epiano` — 8 голосов (`EPIANO_NUM_VOICES`).
3. **Лицензирование DaisySP-LGPL:** Реализация струны `Pluck` (только для `PURRMIDI_SYNTH=pluck`) находится в подмодуле `DaisySP-LGPL` под лицензией LGPLv3. `epiano` и `sine` от DaisySP не зависят.
4. **Интеграция с прошивкой STM32:** прошивка и Windows-приложение используют один и тот же интерфейс `synth_engine.h` и один и тот же `MIDI_Dispatch`, поэтому звучание, отлаженное на ПК, совпадает с прошивкой (при том же `PURRMIDI_SYNTH`).
5. **Названия CC в мониторе:** в режиме `--monitor` подписи контроллеров (`Damp`, `Decay`, `Sustain`) соответствуют синтезатору `pluck`; для `epiano` смысл CC описан в [EPIANO_SYNTH.md](EPIANO_SYNTH.md).

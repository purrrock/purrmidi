# PurrMidi

**PurrMidi** — экспериментальный автономный MIDI-синтезатор на базе STM32: подключаете USB MIDI-клавиатуру, звук генерирует микроконтроллер и выводит его через внешний аудио ЦАП. Основное направление — качественный звук фортепиано.

В проекте есть прошивка STM32H743, Windows-приложение для проверки звуковых движков на ПК, шесть звуковых движков (электропиано, нейлоновая струна, лид, орган, скрипка, SoundFont) и хостовые автоматические тесты. Проект развивается и пока не является законченным серийным продуктом.

## Аппаратное обеспечение

* **Микроконтроллер:** WeAct STM32H743VIT6 Mini (Cortex-M7).
* **Тактирование:** HSE 25 МГц; CPU 480 МГц, USB 48 МГц, аудио 48 кГц.
* **Аудио ЦАП:** PCM5102A, стереовыход через SAI1 Block A, 16 бит, два слота, кадр 32 бита.
* **MIDI-вход:** USB MIDI-клавиатура через USB OTG FS Host.
* **Кнопка управления:** User Key K1 (PC13) на плате микроконтроллера для переключения инструмента.
* **Дисплей:** TFT ST7735 160×80, SPI4 — статус MIDI, активные ноты, название аккорда и выбранный инструмент.
* **Внешняя флеш-память:** Winbond W25Q64 (8 МБ), QUADSPI Bank1. Используется в режиме прямого адресации Memory-Mapped (XIP) для хранения и мгновенного исполнения/чтения сэмплов SoundFont.
* **Карта памяти:** microSD через SDMMC1, 4-битный режим (FAT32). Используется **только при запуске** и **только для чтения**: для автоматической записи/обновления SoundFont-банка `0:/SNDFNT.SF2` во внешнюю QSPI Flash. Во время игры SD-карта не нужна и не используется.

Физические подключения и распиновка описаны в `PINOUT.md`.

Физические подключения и распиновка описаны в `PINOUT.md`.

## Основы работы

**Подключение.** USB MIDI-клавиатуру подключите к USB OTG разъёму платы (прибор работает как USB Host), аудиовыход PCM5102A — к усилителю или наушникам. Включите питание: запускается E-Piano. SD-карта для игры не нужна.

**Выбор инструмента.** Сообщением MIDI Program Change или кнопкой **User Key K1** на плате.

| Program Change | Инструмент | Что это |
| -------------: | ---------- | ------- |
| `0` | **E-Piano** | FM-электропиано (по умолчанию) |
| `1` | **Pluck** | Нейлоновая гитарная струна |
| `2` | **Lead** | Аналоговый лид |
| `3` | **Organ** | Тонколесный орган |
| `4` | **Violin** | Скрипка |
| `5…` | **SoundFont** | Пресеты банка SoundFont2: `5` — первый, `6` — второй и т. д. |
| `127` | — | Следующий инструмент по кругу (так же работает K1) |

Название инструмента и активные ноты показываются на дисплее. Выбор не запоминается: после включения всегда E-Piano.

**Управление с клавиатуры.**

| Что | MIDI |
| --- | ---- |
| Общая громкость | CC7 (работает на всех инструментах) |
| Педаль сустейна | CC64 |
| Pluck: яркость / затухание | CC71, CC74 / CC72; мод-колесо (CC1) делает звук ярче, отпущенное колесо тембр не меняет |
| Organ: регистры, хорус | CC20–28 (drawbars), CC70 (хорус вкл/выкл), CC71/72 (скорость и глубина хоруса) |
| Violin: вибрато, смычок | CC1 вибрато, CC70/71/75 положение, прижим и шум смычка, Pitch Bend, aftertouch |
| E-Piano, Lead | см. [docs/EPIANO_SYNTH.md](docs/EPIANO_SYNTH.md), [docs/ENGINES.md](docs/ENGINES.md) |

**Свой SoundFont.** Положите файл банка в корень microSD-карты под именем `SNDFNT.SF2`, вставьте карту и включите прибор: банк автоматически запишется во флеш-память (30–60 секунд, ход виден на дисплее). Дальше карта не нужна. Подробности и диагностика — в [docs/SD_CARD_AND_QSPI.md](docs/SD_CARD_AND_QSPI.md).

**Если клавиатуру отключили,** звук сбрасывается, «залипших» нот не остаётся. После аппаратного сбоя или зависания прибор перезапускается сам.

## Сборка

### Получение исходников

```bash
git clone https://github.com/purrrock/purrmidi
```

Подмодулей git в проекте нет: всё, что нужно для сборки, лежит в самом репозитории (сторонний код — в `Middlewares/Third_Party/` и `Platform/display/st7735/`), поэтому флаг `--recursive` не нужен, а клон занимает несколько десятков мегабайт.

### Прошивка STM32

```bash
cmake --preset Debug
cmake --build --preset Debug
```

Для релизной сборки:

```bash
cmake --preset Release
cmake --build --preset Release
```

### Хостовые тесты

```bash
cmake --preset host-tests
cmake --build --preset host-tests
ctest --test-dir build/host-tests --output-on-failure
```

### Windows-версия

В Windows с установленным CMake и компилятором MSVC:

```bash
cmake --preset win-msvc-release
cmake --build --preset win-msvc-release
```

Инструкции и дополнительные сведения: `docs/WINDOWS_BUILD.md`.

## Структура репозитория

```text
Core/                 Только код, сгенерированный CubeMX, и main.c (пользовательский код — в USER CODE блоках)
Engine/midi/          MIDI-очередь, диспетчер, разбор USB-MIDI пакетов, детектор аккордов (общий код прошивки, тестов и Windows)
Engine/synth/         Звуковые движки и диспетчер SynthEngine
Platform/midi_usb/    USB-MIDI приём, класс usbh_midi и восстановление порта (STM32)
Platform/display/     Интерфейс TFT ST7735 (STM32) и скопированный драйвер ST7735 (st7735/)
Platform/system/      Watchdog, диагностика, инициализация ОЗУ (STM32)
Platform/sd/          Работа с microSD (sd_storage, STM32)
Platform/qspi_xip/    QSPI Flash, XIP, MPU, загрузчик SoundFont с SD (STM32)
Platform/host/        Хост-реализация загрузчика SoundFont (банк из файла в ОЗУ) для тестов и Windows-приложения
Platform/windows/     Windows-приложение для проверки движков на ПК
USB_HOST/, FATFS/     Настройки USB Host и FatFs (CubeMX)
Drivers/, Middlewares/  HAL/CMSIS, USB Host Library, FatFs и сторонние библиотеки (подмодулей git нет)
SoundFonts/           Примеры SoundFont-банков и скрипт sf2_shrink.py
tests/                Хостовые тесты (CMake preset host-tests)
docs/                 Документация по подсистемам
tools/                Вспомогательные скрипты (openocd-run.cmd)
```

## Документация

| Документ | О чём |
| -------- | ----- |
| [docs/ENGINES.md](docs/ENGINES.md) | Звуковые движки, правила Program Change, общая громкость, программный интерфейс |
| [docs/PLUCK_SYNTH.md](docs/PLUCK_SYNTH.md) | Как устроена нейлоновая струна, параметры, измерения |
| [docs/EPIANO_SYNTH.md](docs/EPIANO_SYNTH.md) | FM-электропиано |
| [docs/SOUNDFONT_QSPI_SYNTH.md](docs/SOUNDFONT_QSPI_SYNTH.md) | Синтезатор SoundFont и QSPI Flash (XIP) |
| [docs/SD_CARD_AND_QSPI.md](docs/SD_CARD_AND_QSPI.md) | Обновление банка с SD-карты, настройка SDMMC1, диагностика |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Путь MIDI-событий, DMA, D-Cache, карта памяти, арена голосов, общие компоненты |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Что реализовано, тесты и CI, этапы и статус разработки |
| [docs/WINDOWS_BUILD.md](docs/WINDOWS_BUILD.md) | Сборка и запуск Windows-приложения |
| [docs/USB_Enumeration_fix.md](docs/USB_Enumeration_fix.md) | Исправление зависания энумерации USB |
| [docs/PCM5102A_PINOUT.md](docs/PCM5102A_PINOUT.md) | Подключение ЦАП PCM5102A |

## Лицензии

* **Сторонние компоненты.** В проекте используются HAL/CMSIS, USB Host Library (ST), FatFs, TinySoundFont, часть DaisySP (`Oscillator`, `Svf`, `Adsr`), bfreeOrgan2 и другие. Полный перечень, лицензии и условия — в [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
* **Лицензия собственного кода PurrMidi — MIT** (файл [`LICENSE`](LICENSE)). Кода под LGPL/GPL в сборке нет: библиотека DaisySP-LGPL, которая раньше линковалась ради `Pluck`, из проекта удалена. Открытый вопрос — лицензия растрового ASCII-шрифта дисплея (см. `THIRD_PARTY_NOTICES.md`).
* **SoundFont-банки** из `SoundFonts/` и `tests/SNDFNT.SF2`: источник и условия распространения для части файлов не подтверждены — см. таблицу в `THIRD_PARTY_NOTICES.md`.

## Репозиторий

Исходный код: https://github.com/purrrock/purrmidi

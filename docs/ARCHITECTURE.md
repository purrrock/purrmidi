# Архитектура программного обеспечения

Документ описывает, как устроен PurrMidi внутри: путь MIDI-событий, аудиотракт, память STM32H743 и общие компоненты звуковых движков. Основы работы — в [README](../README_RU.md), звуковые движки — в [ENGINES.md](ENGINES.md), SoundFont и QSPI — в [SOUNDFONT.md](SOUNDFONT.md).

## Аппаратная платформа

STM32H743VIT6 (Cortex-M7): CPU 480 МГц, аудио 48 кГц. ЦАП PCM5102A на SAI1 Block A (16 бит, два слота, кадр 32 бита). Банк SoundFont лежит во внешней QSPI Flash и читается в режиме XIP; microSD используется только при запуске и
только для чтения. Выводы и подключение — в [HARDWARE.md](HARDWARE.md).

## Общая схема

Проект разделяет обработку MIDI, звуковые движки и аппаратный аудиовывод. Общий интерфейс движков находится в `Engine/synth/synth_engine.h`, реализация диспетчера — в `Engine/synth/synth_engine.cpp`.

Основной путь MIDI-событий:

```text
USB MIDI-клавиатура
        │
        ▼
USB Host / MIDI-декодирование
        │
        ▼
MIDI-очередь событий
        │
        ▼
MIDI_Dispatch()
        │
        ▼
SynthEngine
        │
        ▼
Заполнение стереобуфера
        │
        ▼
SAI1 Block A + DMA
        │
        ▼
PCM5102A
        │
        ▼
Аудиовыход
```

Разбор 4-байтовых USB-MIDI пакетов вынесен в чистую функцию `UsbMidi_PacketToEvent()` (`Engine/midi/usb_midi_packet.c`): она пропускает только сообщения голосового канала, а SysEx, MIDI Clock, Active Sensing и прочие системные сообщения отбрасывает; байты данных маскируются до 7 бит.

MIDI-диспетчер обрабатывает Note On, Note Off, Control Change, Program Change, Pitch Bend, Channel Pressure и Polyphonic Key Pressure. Pitch Bend сейчас реально звучит в E-Piano и Violin (±2 полутона) и SoundFont (по RPN 0, по умолчанию ±2); остальные движки его игнорируют. Channel Pressure (aftertouch) использует только Violin (усиливает смычок); Polyphonic Key Pressure маршрутизируется к движку, но пока ни одним не используется. Если USB-клавиатуру отключили, активный движок сбрасывается (`SynthEngine_Panic()`), чтобы не оставалось «залипших» нот. Note On с velocity `0` трактуется как Note Off. События передаются активному звуковому движку через единый интерфейс.

В аппаратной версии аудиобуфер заполняется в обработчиках завершения половины и полного блока DMA. В Windows-приложении используются тот же код звуковых движков и общий диспетчер, но аудиовывод реализован средствами платформы Windows.

### DMA, D-Cache и память STM32

* Аудиопередача использует SAI1 Block A и DMA2 Stream0 в режиме circular.
* Кольцевой аудиобуфер содержит 256 стереокадров и разделён на две половины.
* Рендер звука выполняется не в прерывании DMA, а в **PendSV** с самым низким приоритетом: прерывание DMA2 Stream0 (приоритет 0) только отмечает, какая половина буфера освободилась (`Engine/audio/audio_halves.c`), и запускает PendSV; рендер блока (до 2,67 мс) вытесняется USB (0), SAI (1), SDMMC (5) и SysTick (14). Если рендер не успевает, пропущенный срок учитывается (`late=` в строке `[AUDIO]`, индикация на дисплее), а устаревшая половина заливается тишиной вместо повтора старого звука. Приоритеты заданы в CubeMX (`PendSV` — 15, `SysTick` — 14).
* **Карта SRAM.** Каждая область отвечает за своё:
  * **DTCM** (128 КБ): `.data`, `.bss`, стек (недоступна DMA);
  * **D2 SRAM** (288 КБ, `0x30000000`): только DMA-буферы DMA1/DMA2 — секция `.dma_buffer` (кольцо SAI1, холст дисплея). **SD сюда нельзя**: IDMA SDMMC1 находится в домене D1 и видит только AXI SRAM (ST AN5200); MPU: Normal Non-cacheable, DMA и CPU видят одни и те же данные без операций с кэшем;
  * **AXI SRAM** (512 КБ, `0x24000000`): **арена голосов** `.synth_arena` (72 КБ, общая для всех движков) и **куча malloc** `.axi_heap` (320 КБ: метаданные TinySoundFont, USB). MPU: Normal Write-back + write-allocate, кэшируется D-cache; DMA сюда не работает. Занято около 80%.
* **Арена голосов** (`Engine/synth/synth_arena.h`). Одновременно звучит один движок, поэтому голоса Pluck (66 КБ), Violin (≈41 КБ) и Lead (2 КБ) делят одну область размером с самый большой набор вместо раздельной памяти (арена 72 КБ; размер задают `SYNTH_ARENA_BYTES` и `ASSERT` в `STM32H743xx_FLASH.ld`). Движок создаёт голоса в арене в своём `Init()` (`SynthArena_NewArray`, явные конструкторы), а движок, у которого арену забрал другой, молчит. Увеличить арену нужно, только если новый движок не помещается: `static_assert` и `ASSERT` линкера подскажут (`SYNTH_ARENA_BYTES`).
* **Общая инфраструктура движков** (`Engine/synth/synth_common.h`): очередь событий с резервом под Note Off и протоколом аварийного восстановления (`synth::NoteInput`), мягкий ограничитель `synth::SoftLimit`, таблица частот `synth::MidiFreq`. Звук движков проверяется бит-в-бит тестом `test_engine_golden`.
* Для быстрой и эффективной работы QSPI Flash в режиме Memory-Mapped (XIP) включён **D-Cache** для MPU-региона `0x90000000...0x907FFFFF` (8 МБ, read-only, write-through). Это позволяет подтягивать данные сэмплов TinySoundFont 32-байтными строками кэша без задержек.
* Буферы DMA1/DMA2 лежат в D2 SRAM (Normal Non-cacheable), а AXI SRAM кэшируется write-back: линии задержки физических моделей (Violin, Pluck) читаются последовательно и хорошо ложатся в 16-КБ D-cache. Буферы SD (bounce-буфер и буфер загрузчика SF2) лежат в секции `.sd_dma_buffer` (AXI SRAM, выровнены по 32 байта), драйвер SD инвалидирует D-cache до и после каждой передачи; прямая DMA-передача разрешена в любой выровненный буфер AXI SRAM, остальное идёт через bounce-буфер. **Любой новый DMA-буфер DMA1/DMA2 обязан лежать в `.dma_buffer`, любой буфер SDMMC1 — в `.sd_dma_buffer`.**
* Секции `.dma_buffer`, `.synth_arena` и `.axi_heap` (NOLOAD) не обнуляются startup-кодом и не получают начальных значений (объекты с инициализаторами в них терялись бы), а AXI SRAM защищена ECC (слово 64 бита): неполная запись в неинициализированное слово может вызвать ECC-ошибку. Поэтому `Platform/system/ram_init.c` до запуска C++-конструкторов (через `.preinit_array`) записывает нули 64-битными словами (`STRD`) во всю AXI SRAM и всю D2 SRAM (границы — символы линкер-скрипта), а объекты в арене создаются явно. Startup-файл CubeMX менять не нужно.
* **Контракт XIP.** Любая индексная операция QSPI (чтение, стирание, запись) выключает Memory-Mapped режим и закрывает MPU-окно `0x90000000`; чтение банка в этот момент вызвало бы MemManage. Поэтому драйвер сбрасывает признак `QSPIFlash_IsMapped()` до закрытия окна и устанавливает его после открытия, а синтезатор SoundFont перед каждым блоком проверяет `SF2Loader_BankReadable()` и при закрытом окне отдаёт тишину. Загрузчик/обновление банка выполняется только при старте.

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
SoundFonts/           Примеры SoundFont-банков и скрипт sf2_shrink.py (эталон алгоритма)
site/                 Сайт (GitHub Pages) и онлайн-инструмент уменьшения SF2 (site/sf2/, порт sf2_shrink.py на JS)
tests/                Хостовые тесты (CMake preset host-tests)
docs/                 Документация (см. список в README)
enclosure/            Файлы корпуса и чертежи плат
tools/                Вспомогательные скрипты (openocd-run.cmd, check_firmware_size.sh, check_firmware_flags.sh)
```

## Диагностические флаги сборки

В обычном Release диагностика аудио выключена (нет периодических строк `[AUDIO]`). Для измерения времени рендера соберите отдельный
Release; нужны оба флага (`main.c` на C, `soundfont_synth.cpp` на C++):

```bash
cmake --preset Release -B build/Release-diag -DCMAKE_C_FLAGS="-DAUDIO_DIAG_UART=1" -DCMAKE_CXX_FLAGS="-DAUDIO_DIAG_UART=1 -DSF2_DIAG_STATS=1"
cmake --build build/Release-diag
```

Строки `[AUDIO] <движок>: fill last=… max=… us (NN% of 2666 us) >80%: … >100%: … shed=… late=…` появляются при росте максимума или
нарушении порогов; `[AUDIO] RENDER OVERRUN` означает, что блок не уложился в бюджет. Расшифровка полей `[SF2]` — в
[SOUNDFONT.md](SOUNDFONT.md#диагностика). Проверка размера прошивки: `bash tools/check_firmware_size.sh build/Release/purrmidi.elf`.

Для отладки по SWD соберите с `-DPURRMIDI_FAULT_HALT=1`: `Error_Handler()` и обработчики сбоев остановятся в бесконечном цикле
вместо перезапуска (по умолчанию `Error_Handler()` печатает отчёт, ждёт около секунды и перезапускает прибор).

## USB Host: восстановление отключённого порта

Некоторые USB MIDI-устройства (например, клавиатуры на чипах Jieli) вскоре после сброса порта хостом на мгновение отпускают и
снова подтягивают D+. Ядро OTG видит `DISCINT + PCDET` и аппаратно сбрасывает `HPRT0.PENA`, порт отключён (нет SOF и SETUP), а
стек USB Host продолжает считать устройство подключённым. Первый control-запрос энумерации никогда не завершается:

```text
GET_DEV_DESC_8 START
GET_DEV_DESC_8 BUSY
GET_DEV_DESC_8 BUSY
...            (без HCINT/XFRC и URB_DONE)
```

`USB_PortLostRecover()` (`Platform/midi_usb/usb_port_recover.c`, вызывается из главного цикла `main.c`) ищет это состояние:
`is_connected != 0`, стек находится между `HOST_ENUMERATION` и `HOST_CLASS`, а `HPRT0.PENA == 0`. Тогда выставляются
`is_disconnected` и `is_ReEnumerated`, вызывается `USBH_Stop()`, и штатный путь `USBH_Process()` (деинициализация класса и
конечного автомата, `USBH_Start()`) заново запускает сброс порта и энумерацию без физического переподключения. Перед этим
делается блокирующая пауза 300 мс (редкое событие, сторожевой таймер её переживает). Счётчик срабатываний —
`USB_PortLostRecoverCount()`.

Если энумерация началась, но первый control-запрос вечно `BUSY`, сначала проверьте `HPRT0.PENA`.

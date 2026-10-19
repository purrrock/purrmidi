# Сторонние компоненты и лицензии

Собственный код PurrMidi (каталоги `Core/`, `Engine/`, `Platform/`, `USB_HOST/`, `FATFS/`, `tests/`, `docs/`)
распространяется на условиях **MIT License** (файл `LICENSE` в корне репозитория).

Ниже перечислены сторонние компоненты, исходный код которых входит в репозиторий или подтягивается
подмодулями. Колонка «Где используется» показывает, попадает ли компонент в итоговую прошивку.
Тексты лицензий лежат рядом с исходниками (путь указан в таблице).

## Компоненты, попадающие в прошивку и/или Windows-приложение

| Компонент | Путь | Лицензия | Где используется |
|---|---|---|---|
| **STM32H7xx HAL Driver** (STMicroelectronics) | `Drivers/STM32H7xx_HAL_Driver/` (`LICENSE.md`) | BSD-3-Clause | прошивка |
| **CMSIS** (Arm) | `Drivers/CMSIS/` (`LICENSE.txt`) | Apache-2.0 | прошивка |
| **STM32 USB Host Library** (STMicroelectronics) | `Middlewares/ST/STM32_USB_Host_Library/` (`LICENSE.txt`) | ST SLA0044 («Ultimate Liberty Software License Agreement»): использование и распространение допускаются только в составе продуктов на микроконтроллерах/микропроцессорах ST — для STM32H743 это выполняется; не переносите этот код на МК других производителей | прошивка |
| **FatFs R0.12c** (ChaN) | `Middlewares/Third_Party/FatFs/` (условия — в заголовке `ff.c`) | BSD-подобная, 1 условие: сохранять копирайт и отказ от гарантий в исходниках | прошивка |
| **ST7735 BSP-драйвер** (STMicroelectronics) — только файлы `st7735.c`, `st7735_reg.c`, `st7735.h`, `st7735_reg.h` | `Middlewares/Third_Party/MiniSTM32H7xx/SDK/HAL/STM32H743/08-DCMI2LCD/Drivers/BSP/ST7735/` | BSD-3-Clause (указано в заголовках файлов) | прошивка. **Внимание:** остальной репозиторий WeActStudio/MiniSTM32H7xx лицензирован GPL-3.0 (`MiniSTM32H7xx/LICENSE`) — эти файлы в сборку **не входят** и использоваться в прошивке не должны |
| **TinySoundFont** (Bernhard Schelling) — с доработками PurrMidi (чтение PCM «на месте», `tsf_set_smpl_pcm16`) | `Middlewares/Third_Party/TinySoundFont/tsf.h` (лицензия — в заголовке файла) | MIT | прошивка, Windows |
| **DaisySP** (Electrosmith) — в прошивку линкуются `Oscillator`, `Svf`, `Adsr` | `Middlewares/Third_Party/DaisySP/` (`LICENSE`) | MIT | прошивка, Windows |
| **DaisySP-LGPL** (Electrosmith) — вложенный подмодуль DaisySP; в прошивку линкуется только `Pluck` (`PhysicalModeling/pluck.cpp`, движок Pluck) | `Middlewares/Third_Party/DaisySP/DaisySP-LGPL/` (`LICENSE`) | **LGPL-2.1** | прошивка (статически), Windows — см. примечание ниже |
| **bfreeOrgan2 / flexynth** (Franco Caspe) | `Middlewares/Third_Party/bfreeOrgan2/` (`LICENSE.txt`, `NOTICE`) | Apache-2.0 | прошивка (движок Organ), Windows |
| **RtMidi** (Gary Scavone) — скачивается CMake при сборке Windows-приложения | `FetchContent`, см. `Platform/windows/CMakeLists.txt` | MIT-подобная (лицензия RtMidi) | только Windows |

### Примечание про LGPL-2.1 (DaisySP-LGPL)

Прошивка статически линкуется с кодом DaisySP-LGPL: по карте линковки это единственный объект `pluck.cpp` (`daisysp::Pluck`, движок Pluck).
Остальные используемые классы DaisySP (`Oscillator`, `Svf`, `Adsr`) — MIT.
Для распространения **бинарных** файлов прошивки (`.elf`, `.bin`, `.hex`) это означает:

1. вместе с бинарником нужно поставлять текст LGPL-2.1 (`DaisySP-LGPL/LICENSE`) и указать, что прошивка содержит DaisySP-LGPL;
2. получатель должен иметь возможность заменить эту библиотеку собственной версией и пересобрать прошивку. Публичный репозиторий
   с воспроизводимой сборкой (`cmake --preset Release`, подмодули зафиксированы на конкретных коммитах) это условие выполняет;
   при раздаче готовых бинарников ссылайтесь на тот коммит репозитория, из которого они собраны.

LGPL не требует открывать остальной код PurrMidi, но не отменяет требований п. 1–2.

### Примечание про шапки «Copyright STMicroelectronics» в `Core/` и `USB_HOST/`

Файлы, сгенерированные STM32CubeMX, содержат стандартную шапку ST («licensed under terms that can be found in the LICENSE file…
If no LICENSE file comes with this software, it is provided AS-IS»). Сгенерированный ST код (init-функции, `stm32h7xx_it.c`,
`usb_host.c` и т. п.) подпадает под лицензию ST; код PurrMidi, добавленный в секции `USER CODE`, — под лицензию проекта.

## Подмодули, которые не входят в сборку

| Компонент | Путь | Лицензия | Статус |
|---|---|---|---|
| **STK** (Synthesis ToolKit) | `Middlewares/Third_Party/stk/` (`LICENSE`) | MIT-подобная (STK License) | не компилируется ни в прошивке, ни в тестах, ни в Windows-версии — подмодуль сохранён как справочный пример кода. Модель скрипки (`Engine/synth/violin_synth.cpp`) написана заново по описанию волноводной модели смычковой струны (J. O. Smith, 1992; та же схема в STK `Bowed`), код STK не копировался |
| **Mutable Instruments eurorack** (Émilie Gillet) и вложенный **stmlib** | `Middlewares/Third_Party/eurorack/` | MIT | не компилируется (раньше — движок Dulcimer, удалён); подмодуль сохранён как справочный пример кода |
| **MiniSTM32H7xx** — всё, кроме 4 файлов драйвера ST7735 (см. выше) | `Middlewares/Third_Party/MiniSTM32H7xx/` | **GPL-3.0** | в сборку не входит; подмодуль сохранён как справочный пример кода |

## SoundFont-банки и тестовые данные

| Файл | Происхождение / лицензия |
|---|---|
| `SoundFonts/Yamaha C2 Piano.sf2` | Внутренние метаданные файла: «000 Florestan Piano», `ICOP = Public Domain` (проект Florestan Piano). Имя файла в репозитории не соответствует содержимому. |
| `SoundFonts/Yamaha S90ES.SF2`, `SoundFonts/Yamaha S90ES lite.SF2`, `tests/SNDFNT.SF2` | Метаданных о копирайте нет. Название указывает на сэмплы Yamaha S90ES; права на распространение в репозитории **не подтверждены**. `tests/SNDFNT.SF2` побайтно совпадает с `Yamaha S90ES lite.SF2`. |

**Требуется действие владельца репозитория:** укажите источник и условия распространения для каждого банка либо уберите
банки из репозитория (в т. ч. из истории git) и храните их отдельно (GitHub Releases, внешняя ссылка). Для тестов лучше использовать
небольшой банк, лицензия которого точно разрешает распространение.

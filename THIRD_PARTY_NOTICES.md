# Сторонние компоненты и лицензии

Собственный код PurrMidi (каталоги `Core/`, `Engine/`, `Platform/`, `USB_HOST/`, `FATFS/`, `tests/`, `docs/`)
распространяется на условиях **MIT License** (файл `LICENSE` в корне репозитория).

Ниже перечислены сторонние компоненты, исходный код которых входит в репозиторий. Подмодулей git в проекте нет.
Колонка «Где используется» показывает, попадает ли компонент в итоговую прошивку.
Тексты лицензий лежат рядом с исходниками (путь указан в таблице).

## Компоненты, попадающие в прошивку и/или Windows-приложение

| Компонент | Путь | Лицензия | Где используется |
|---|---|---|---|
| **STM32H7xx HAL Driver** (STMicroelectronics) | `Drivers/STM32H7xx_HAL_Driver/` (`LICENSE.md`) | BSD-3-Clause | прошивка |
| **CMSIS** (Arm) | `Drivers/CMSIS/` (`LICENSE.txt`) | Apache-2.0 | прошивка |
| **STM32 USB Host Library** (STMicroelectronics) | `Middlewares/ST/STM32_USB_Host_Library/` (`LICENSE.txt`) | ST SLA0044 («Ultimate Liberty Software License Agreement»): использование и распространение допускаются только в составе продуктов на микроконтроллерах/микропроцессорах ST — для STM32H743 это выполняется; не переносите этот код на МК других производителей | прошивка |
| **FatFs R0.12c** (ChaN) | `Middlewares/Third_Party/FatFs/` (условия — в заголовке `ff.c`) | BSD-подобная, 1 условие: сохранять копирайт и отказ от гарантий в исходниках | прошивка |
| **ST7735 BSP-драйвер** (STMicroelectronics) — файлы `st7735.c`, `st7735_reg.c`, `st7735.h`, `st7735_reg.h`, скопированы без изменений из WeActStudio/MiniSTM32H7xx (коммит `0425ea9`) | `Platform/display/st7735/` | BSD-3-Clause (указано в заголовках файлов) | прошивка. Остальной репозиторий WeActStudio/MiniSTM32H7xx лицензирован GPL-3.0 и в проект не входит |
| **Растровые ASCII-шрифты 12x6 и 16x8** (таблицы `asc2_1206`, `asc2_1608`) — скопированы без изменения данных из `font.h` того же репозитория MiniSTM32H7xx | `Platform/display/font_ascii.h` | **не определена**: в исходном `font.h` нет заголовка с лицензией, а репозиторий-источник — GPL-3.0. Это распространённый шрифт из демо-кодов для TFT-дисплеев, но происхождение и условия не подтверждены | прошивка (надписи на дисплее). См. примечание ниже |
| **TinySoundFont** (Bernhard Schelling) — с доработками PurrMidi (чтение PCM «на месте», `tsf_set_smpl_pcm16`) | `Middlewares/Third_Party/TinySoundFont/tsf.h` (лицензия — в заголовке файла) | MIT | прошивка, Windows |
| **DaisySP** (Electrosmith) — урезанная копия: только `Oscillator`, `Svf`, `Adsr` и `Utility/dsp.h`, без изменений, коммит `599511b` (V1.0.0-25) | `Middlewares/Third_Party/DaisySP_lite/` (`LICENSE`, `README.md`) | MIT | прошивка, Windows |
| **bfreeOrgan2 / flexynth** (Franco Caspe) | `Middlewares/Third_Party/bfreeOrgan2/` (`LICENSE.txt`, `NOTICE`) | Apache-2.0 | прошивка (движок Organ), Windows |
| **RtMidi** (Gary Scavone) — скачивается CMake при сборке Windows-приложения | `FetchContent`, см. `Platform/windows/CMakeLists.txt` | MIT-подобная (лицензия RtMidi) | только Windows |

### Примечание про шрифт дисплея

Таблицы `asc2_1206` и `asc2_1608` в `Platform/display/font_ascii.h` — единственный элемент в проекте, лицензию которого подтвердить не удалось
(в исходном файле нет заголовка с условиями, репозиторий-источник — GPL-3.0). Пока это так, не раздавайте прошивку как «полностью свободную от
вопросов по лицензиям». Варианты решения: заменить шрифт на таблицу с известной лицензией (например, Terminus, SIL OFL-1.1, или font8x8 из общественного
достояния; формат данных описан в заголовке `font_ascii.h`) либо нарисовать собственный шрифт.

### История: LGPL

Раньше в прошивку статически линковался один объект LGPL-2.1: `daisysp::Pluck` из DaisySP-LGPL. Движок Pluck переписан на собственную реализацию,
каталог DaisySP-LGPL удалён из проекта, и по карте линковки Release-сборки в прошивке остаются только объекты `adsr`, `svf`, `oscillator` (MIT).
Требований LGPL-2.1 к текущей версии проекта нет.

### Примечание про шапки «Copyright STMicroelectronics» в `Core/` и `USB_HOST/`

Файлы, сгенерированные STM32CubeMX, содержат стандартную шапку ST («licensed under terms that can be found in the LICENSE file…
If no LICENSE file comes with this software, it is provided AS-IS»). Сгенерированный ST код (init-функции, `stm32h7xx_it.c`,
`usb_host.c` и т. п.) подпадает под лицензию ST; код PurrMidi, добавленный в секции `USER CODE`, — под лицензию проекта.

## Удалённые из репозитория справочные подмодули

Эти подмодули раньше лежали в `Middlewares/Third_Party/` только как справочные примеры, ни в прошивку, ни в тесты, ни в Windows-версию не входили,
и из проекта убраны вместе с каталогами (около 620 МБ при клонировании с `--recursive`).

| Компонент | Лицензия | Комментарий |
|---|---|---|
| **STK** (Synthesis ToolKit) | MIT-подобная (STK License) | Модель скрипки (`Engine/synth/violin_synth.cpp`) написана заново по описанию волноводной модели смычковой струны (J. O. Smith, 1992; та же схема в STK `Bowed`), код STK не копировался |
| **Mutable Instruments eurorack** (Émilie Gillet) и вложенный **stmlib** | MIT | Раньше использовался для движка Dulcimer, он давно удалён |
| **MiniSTM32H7xx** (WeActStudio) — всё, кроме файлов, перечисленных выше | **GPL-3.0** | Из этого репозитория взяты только драйвер ST7735 и таблицы шрифта (см. выше) |

## SoundFont-банки и тестовые данные

| Файл | Происхождение / лицензия |
|---|---|
| `SoundFonts/Yamaha C2 Piano.sf2` | Внутренние метаданные файла: «000 Florestan Piano», `ICOP = Public Domain` (проект Florestan Piano). Имя файла в репозитории не соответствует содержимому. |
| `SoundFonts/Yamaha S90ES.SF2`, `SoundFonts/Yamaha S90ES lite.SF2`, `tests/SNDFNT.SF2` | Метаданных о копирайте нет. Название указывает на сэмплы Yamaha S90ES; права на распространение в репозитории **не подтверждены**. `tests/SNDFNT.SF2` побайтно совпадает с `Yamaha S90ES lite.SF2`. |

**Требуется действие владельца репозитория:** укажите источник и условия распространения для каждого банка либо уберите
банки из репозитория (в т. ч. из истории git) и храните их отдельно (GitHub Releases, внешняя ссылка). Для тестов лучше использовать
небольшой банк, лицензия которого точно разрешает распространение.

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
| **STM32 USB Host Library** (STMicroelectronics) | `Middlewares/ST/STM32_USB_Host_Library/` (`LICENSE.txt`) | ST SLA0044 («Ultimate Liberty Software License Agreement»): использование и распространение допускаются только в составе продуктов на микроконтроллерах/микропроцессорах ST | прошивка |
| **FatFs R0.12c** (ChaN) | `Middlewares/Third_Party/FatFs/` (условия — в заголовке `ff.c`) | BSD-подобная, 1 условие: сохранять копирайт и отказ от гарантий в исходниках | прошивка |
| **ST7735 BSP-драйвер** (STMicroelectronics) — файлы `st7735.c`, `st7735_reg.c`, `st7735.h`, `st7735_reg.h`, скопированы без изменений из WeActStudio/MiniSTM32H7xx (коммит `0425ea9`) | `Platform/display/st7735/` | BSD-3-Clause (указано в заголовках файлов) | прошивка. Остальной репозиторий WeActStudio/MiniSTM32H7xx лицензирован GPL-3.0 и в проект не входит |
| **Растровые ASCII-шрифты 12x6 и 16x8** (таблицы `asc2_1206`, `asc2_1608`) — скопированы без изменения данных из `font.h` того же репозитория MiniSTM32H7xx | `Platform/display/font_ascii.h` | **не определена**: в исходном `font.h` нет заголовка с лицензией, а репозиторий-источник — GPL-3.0. Это распространённый шрифт из демо-кодов для TFT-дисплеев, но происхождение и условия не подтверждены | прошивка (надписи на дисплее). См. примечание ниже |
| **TinySoundFont** (Bernhard Schelling) — с доработками PurrMidi (чтение PCM «на месте», `tsf_set_smpl_pcm16`; аварийное гашение голосов; `tsf_kill_all_voices` — мгновенный обрыв всех голосов при сбросе движка) | `Middlewares/Third_Party/TinySoundFont/tsf.h` (лицензия — в заголовке файла) | MIT | прошивка, Windows |
| **DaisySP** (Electrosmith) — урезанная копия: только `Oscillator`, `Svf`, `Adsr` и `Utility/dsp.h`, без изменений, коммит `599511b` (V1.0.0-25) | `Middlewares/Third_Party/DaisySP_lite/` (`LICENSE`, `README.md`) | MIT | прошивка, Windows |
| **bfreeOrgan2 / flexynth** (Franco Caspe) | `Middlewares/Third_Party/bfreeOrgan2/` (`LICENSE.txt`, `NOTICE`) | Apache-2.0 | прошивка (движок Organ), Windows |
| **RtMidi** (Gary Scavone) — скачивается CMake при сборке Windows-приложения | `FetchContent`, см. `Platform/windows/CMakeLists.txt` | MIT-подобная (лицензия RtMidi) | только Windows |

### Примечание про шрифт дисплея

Таблицы `asc2_1206` и `asc2_1608` в `Platform/display/font_ascii.h` — единственный элемент в проекте, лицензию которого подтвердить не удалось
(в исходном файле нет заголовка с условиями, репозиторий-источник — GPL-3.0).

### Примечание про шапки «Copyright STMicroelectronics» в `Core/` и `USB_HOST/`

Файлы, сгенерированные STM32CubeMX, содержат стандартную шапку ST («licensed under terms that can be found in the LICENSE file…
If no LICENSE file comes with this software, it is provided AS-IS»). Сгенерированный ST код (init-функции, `stm32h7xx_it.c`,
`usb_host.c` и т. п.) подпадает под лицензию ST; код PurrMidi, добавленный в секции `USER CODE`, — под лицензию проекта.

## Звуковые банки SoundFont (`SoundFonts/`)

Банки не входят в код прошивки: это отдельные файлы-примеры, которые пользователь может записать на карту microSD как `SNDFNT.SF2`.
В релиз GitHub вместе с прошивкой кладётся только `Florestan/000_Florestan_Piano.sf2` (под именем `SNDFNT.SF2`).
Пользователь вправе записывать на карту любой банк, права на который у него есть.

| Банк | Сведения о лицензии | Статус |
|---|---|---|
| **Florestan** (`Florestan/`, кроме `070_Bassoon_Ethan-Nando.sf2`) — автор Nando Florestan | В заголовке каждого файла (поле `ICOP`) указано **Public Domain**. На странице автора ([архивная копия](https://web.archive.org/web/20251028045959/https://dev.nando.audio/pages/soundfonts.html), сам сайт недоступен) банки названы бесплатными (Free). | подтверждено, входит в релиз (`000_Florestan_Piano.sf2`) |
| `Florestan/070_Bassoon_Ethan-Nando.sf2` | В заголовке: Copyright (c) 2000 Ethan Winer. This SoundFont may be freely distributed as long as the author name and copyright notice are not removed. | Подтверждено |


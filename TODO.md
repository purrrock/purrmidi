# TODO

Приоритеты: **P1** — сделать в первую очередь, **P2** — желательно, **P3** — мелочи.

## 1. Лицензии и состав репозитория


## 2. Ошибки и расхождения кода с документацией

- [ ] **P1. `AUDIO_DIAG_UART` включён, хотя комментарий говорит «Off by default»** (`Core/Src/main.c:98`). Это след коммита
  «Temporary return AUDIO_DIAG_UART 1 for debug sound». Раз со звуком всё хорошо, вернуть `0` для Release, иначе
  главный цикл печатает статистику рендера в UART.
- [ ] **P1. Неверный путь запасного банка для хост-сборки.** В `Platform/host/sf2_loader_host.cpp:72`,
  `Platform/host/sf2_loader_host.h:24` и `Platform/windows/main_win.cpp:294` указан `SoundFonts/Yamaha S90ES lite.SF2`, а файл
  лежит в `SoundFonts/Piano/`. Этот вариант поиска не срабатывает никогда (работает только `tests/SNDFNT.SF2`). Исправить путь.
- [ ] **P2. `THIRD_PARTY_NOTICES.md` ссылается на несуществующий файл** `SoundFonts/Yamaha C2 Piano.sf2` (сейчас это
  `SoundFonts/Florestan/000_Florestan_Piano.sf2`).
- [ ] **P2. Арена голосов 144 КБ вместо 72 КБ.** `docs/ARCHITECTURE.md` сам называет 144 КБ остатком от удалённого Dulcimer.
  Проверено сборкой: при `SYNTH_ARENA_BYTES = 72 КБ` (`Engine/synth/synth_arena.h`) и `ASSERT(... >= 72K)` в
  `STM32H743xx_FLASH.ld` прошивка собирается (все `static_assert` на размер голосов проходят), 18 хост-тестов зелёные, а RAM
  падает с 93,75 % до 79,69 % (data+bss 550 500 → 476 772 байта). В патч это **не включено**: Pluck (67 КБ) ложится в 72 КБ с
  запасом около 5 КБ, поэтому сначала нужно проверить на плате все движки подряд, включая быстрые переключения Program Change.
  Освободившуюся память можно отдать куче SoundFont (`_Axi_Heap_Size`).
- [ ] **P2. `SF2_DIAG_STATS` по умолчанию `1`** (`Engine/synth/soundfont_synth.cpp:47`). Статистика собирается в аудио-контексте
  на каждом блоке. Задать `0` для Release.
- [ ] **P2. Release-прошивка не попадает в артефакты CI.** `host-ci.yml` загружает только `build/Debug/purrmidi.elf`, хотя на
  приборе работает Release (для аудио-кода `-O2`). Выгружать `build/Release/purrmidi.elf` (и `.bin`/`.hex`).
- [ ] **P2. Windows-сборка в CI только собирается, тесты не запускаются.** Тесты включены по умолчанию
  (`PURRMIDI_BUILD_TESTS=ON`), но в job `host-build-windows` нет шага `ctest` (и нет `testPresets` для `win-msvc-release`).
- [ ] **P3. `Error_Handler()` отключает прерывания и зацикливается**, перезапуск происходит только по IWDG (около 1,5 с).
  Работает, но неочевидно. Либо написать это в комментарии, либо вызывать `NVIC_SystemReset()` после вывода диагностики
  (`Diag_EnterPanic()` уже сбросил буфер).
- [ ] **P3. Двойной SW_RESET при инициализации ST7735** (`Platform/display/st7735/st7735.c:159–163`): результат первой записи
  затирается второй (`ret =`, потом снова `ret =`). Если двойной сброс сделан сознательно, добавить комментарий и учесть оба
  результата; если нет, убрать лишний.
- [ ] **P3. Имена параметров в объявлении и определении различаются** (cppcheck `funcArgNamesDifferent`): `SF2L_Run`
  (`sf2_loader_core.h:116` — `env, report`; `sf2_loader_core.c:229` — `e, rep`) и `Display_SetNoteState`
  (`display.h:72` — `last_note`; `display.c:840` — `note_val`). Привести к одному виду.

## 3. Тестирование и проверка на железе

- [ ] **P3. Санитайзеры только на Linux/GCC.** Прогон под MSVC AddressSanitizer не настроен.

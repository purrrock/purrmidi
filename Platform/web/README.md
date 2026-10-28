# Platform/web — веб-демо

Звуковые движки из `Engine/synth/` в WebAssembly для страницы `site/demo/` (на сайте: «Веб-демо»).

- `purrmidi_web.cpp` — экспортируемые функции: MIDI-события, рендер блока, имена программ, определение аккорда, загрузка SoundFont.
- `sf2_loader_web.cpp` — банк SoundFont лежит в памяти модуля (в прошивке он в QSPI, на ПК в файле).
- `build.sh` — сборка в `site/demo/purrmidi.wasm`. Нужен только Zig: `pip install ziglang==0.17.0`.

Файл `.wasm` и банки в репозиторий не коммитятся: их собирает и копирует `.github/workflows/pages.yml`.
Локально: `bash Platform/web/build.sh`, затем `mkdir -p site/demo/banks`, скопировать туда `.sf2` из `SoundFonts/Florestan/`
(имена в `BANKS` в `site/demo/demo.js`) и открыть сайт через `python3 -m http.server -d site`.
Проверка модуля: `node tests/web/wasm_smoke.mjs`.

# SoundFont в QSPI Flash (XIP)

Банк `SNDFNT.SF2` (до **8 384 512 байт** = 8 МБ − 4 КБ) хранится во внешней QSPI Flash и читается синтезатором
напрямую по адресам `0x90000000...` (Memory-Mapped, XIP). RAM-кэша нет. SD-карта нужна только для обновления банка.

## Что происходит при старте (`SoundFontSynth_InitSF2` -> `SF2Loader_Ensure`)

1. Инициализация QSPI (сброс флеша, JEDEC ID, бит Quad Enable).
2. Есть SD-карта и `0:/SNDFNT.SF2`? Проверка: `RIFF`/`sfbk`, размер, сравнение с заголовком во флеше
   (размер + штамп времени файла; если штамп другой - CRC32). Флеш **не трогается**, если банк тот же.
3. Новый/изменённый файл: стирание заголовка -> стирание данных (64 КБ блоки, хвост 4 КБ секторами) -> запись
   страницами по 256 Б -> проверка CRC32 всего банка **через XIP** -> запись заголовка (коммит).
   Потеря питания в любой момент = нет заголовка = запись повторится при следующем старте с картой.
4. QSPI в Memory-Mapped режиме, открывается кэшируемое MPU-окно, TinySoundFont разбирает метаданные из XIP.

Прогресс виден на дисплее и в UART (`[SF2] ...`). Полная запись 8 МБ занимает порядка 30-60 с.
Если карты нет, а во флеше валидный банк - старт без задержек. Если банка нет нигде - движок SoundFont пропускается
(как и раньше при отсутствии файла).

## Настройка CubeMX (PurrMidi.ioc)

**Connectivity -> QUADSPI**

| Параметр | Значение |
|---|---|
| Mode | **Bank1 with Quad SPI Lines** |
| Clock Prescaler | **2** (80 МГц - надёжный старт); после проверки можно **1** (120 МГц, предел W25Q64 - 133 МГц) |
| FIFO Threshold | 1 |
| Sample Shifting | **Half Cycle Shift** |
| Flash Size | **23** (в сгенерированном коде должно получиться `hqspi.Init.FlashSize = 22`, т.е. 8 МБ) |
| Chip Select High Time | **5 Cycles** |
| Clock Mode | Low |
| Flash ID | 1 |
| Dual Flash | Disabled |

**Пины** (CubeMX назначит сам при выборе режима; проверьте): `PB2` CLK, `PB6` NCS, `PD11` IO0, `PD12` IO1,
`PE2` IO2, `PD13` IO3. С остальной разводкой проекта (SAI, SDMMC, SPI4, USART3) конфликтов нет.
GPIO Speed: **Very High**, Pull: No. Прерывание QUADSPI и DMA не нужны.

**Clock Configuration**: источник QUADSPI - HCLK (в `.ioc` уже `QSPIFreq = 240 МГц`).

**Cortex-M7 / MPU / Cache в CubeMX не включать**: MPU-регионы 1-4 и D-cache включает код (`XipMpu_Config()`,
`SCB_EnableDCache()` в `USER CODE BEGIN Init` в `main.c`). Регион 0 из `MPU_Config()` остаётся как есть.

**Project Manager**: сгенерируйте проект. CubeMX добавит `Core/Src/quadspi.c`, `Core/Inc/quadspi.h`,
`stm32h7xx_hal_qspi.c/.h`, включит `HAL_QSPI_MODULE_ENABLED`, обновит `cmake/stm32cubemx/CMakeLists.txt`.
CMake сам увидит `quadspi.h` и переключится на XIP-бэкенд (в логе: `SoundFont backend: QSPI XIP`).
Принудительно вернуть SD-кэш: `-DPURRMIDI_FORCE_SD_CACHE=ON`.

## Почему D-cache обязателен

Без D-cache каждое чтение `int16_t` из XIP - отдельная QSPI-транзакция (сотни нс), а TinySoundFont читает два
сэмпла на отсчёт каждого голоса. С D-cache строка 32 байта (16 отсчётов) подтягивается одной транзакцией.
Чтобы не ломать DMA, все SRAM (AXI, SRAM1-3, SRAM4) помечены Normal non-cacheable, кэшируется только
окно `0x90000000` (8 МБ, read-only, write-through). Окно включается **только пока QSPI в Memory-Mapped**:
спекулятивные чтения Cortex-M7 в неотображённое окно могут подвесить шину.

## Ограничения

- Размер банка ≤ 8 384 512 байт. Урезайте банк в Polyphone/Swami. (`tests/SNDFNT.SF2` на 27 МБ - только для хост-тестов.)
- Микросхема - Winbond W25Q64 (WeAct H743). Для другой флеши проверьте коды команд, QE-бит и dummy-циклы
  (`QSPIFLASH_READ_DUMMY_CYCLES`).
- Обновление блокирует главный цикл (как и прежняя загрузка с SD); звук в это время не играет.
- При сбое записи старый банк уже стёрт (запасного слота нет): нужен повторный старт с картой.

## Тесты (хост)

`test_sf2_loader_core` - логика на эмуляции NOR-флеша (пропуск записи, перезапись, сбой питания, ошибка проверки,
границы стирания). `test_sf2_xip_host` - реальный TinySoundFont поверх XIP-бэкенда; вывод бит-в-бит совпадает
с файловым кэшем.

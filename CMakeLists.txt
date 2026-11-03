cmake_minimum_required(VERSION 3.22)

option(PURRMIDI_HOST_BUILD "Windows/host app and tests instead of STM32 firmware" OFF)
if(PURRMIDI_HOST_BUILD)
    project(purrmidi_host C CXX)
    enable_testing()
    add_subdirectory(Platform/windows)
    return()
endif()

set(CMAKE_PROJECT_NAME purrmidi)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

project(${CMAKE_PROJECT_NAME} C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Предупреждения компилятора в коде проекта считаются ошибками (в CI включено). Сторонние каталоги
# (bfreeOrgan2, TinySoundFont) подключаются как SYSTEM, их предупреждения не учитываются.
option(PURRMIDI_WERROR "Treat compiler warnings in project code as errors" OFF)

# Main firmware executable
add_executable(${CMAKE_PROJECT_NAME})

# STM32CubeMX-generated sources and libraries
# Частота QSPI (чтение сэмплов SoundFont через XIP): SCLK = HCLK(240 МГц) / (prescaler + 1).
#   2 -> 80 МГц (по умолчанию, как было), 1 -> 120 МГц. Включать 1 только если флеш допускает команду 0xEB на этой частоте
#   (W25Q64JV - до 133 МГц, W25Q64FV - до 104 МГц!). После смены прогнать проверку SF2 (CRC банка, Quad read).
set(PURRMIDI_QSPI_PRESCALER 2 CACHE STRING "QUADSPI ClockPrescaler (SCLK = HCLK / (N+1))")
# Глобально (до add_subdirectory): quadspi.c компилируется в цели из cmake/stm32cubemx, а не в основной.
add_compile_definitions(PURRMIDI_QSPI_PRESCALER=${PURRMIDI_QSPI_PRESCALER})
add_subdirectory(cmake/stm32cubemx)

# Debug: -O0 везде замедляет не только сам синтез, но и все прерывания, которые вытесняют рендер (PendSV имеет
# самый низкий приоритет): USB Host (HAL_HCD, USBH-стек, usbh_midi), SDMMC/QSPI/DMA/SAI HAL, разбор MIDI. Время
# рендера в логе "[AUDIO] ... fill" включает это вытеснение, поэтому в Debug собирается с -O2 весь "рантайм-путь"
# (синтез, DaisySP, HAL, USB, MIDI), а main.c, дисплей и прочее остаются на -O0 -g3 для удобной отладки.
# Отключить (полный -O0): -DPURRMIDI_FAST_DEBUG=OFF.
option(PURRMIDI_FAST_DEBUG "Debug build: compile the audio/IRQ/USB runtime path with -O2 (rest stays -O0)" ON)

# ---------------------------------------------------------------------------
# DaisySP (урезанная копия: только Oscillator, Svf, Adsr для движка Lead)
# ---------------------------------------------------------------------------

set(DAISYSP_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/DaisySP_lite"
)

add_library(DaisySP STATIC
    ${DAISYSP_DIR}/Source/Control/adsr.cpp
    ${DAISYSP_DIR}/Source/Filters/svf.cpp
    ${DAISYSP_DIR}/Source/Synthesis/oscillator.cpp
)

# Передаем аппаратные флаги микроконтроллера (-mcpu, -mthumb) из CubeMX в DaisySP
target_link_libraries(DaisySP PRIVATE stm32cubemx)

# Каталоги заголовков: lead_synth.cpp подключает "Synthesis/oscillator.h", "Filters/svf.h",
# "Control/adsr.h" и "Utility/dsp.h" от корня Source; сами исходники DaisySP включают dsp.h по короткому имени.
# SYSTEM: предупреждения из заголовков сторонней DaisySP не должны попадать в lead_synth.cpp (там действует -Wextra).
target_include_directories(DaisySP SYSTEM PUBLIC
    ${DAISYSP_DIR}/Source
    ${DAISYSP_DIR}/Source/Utility
    ${DAISYSP_DIR}/Source/Synthesis
    ${DAISYSP_DIR}/Source/Filters
    ${DAISYSP_DIR}/Source/Control
)

target_compile_features(DaisySP PUBLIC cxx_std_17)

# DaisySP (Lead: Oscillator/Svf/Adsr) считается на каждом сэмпле в аудио-прерывании: скорость важнее размера.
# Только Release (в Debug остаётся -O0 -g3). Подробности - в блоке "Оптимизация по скорости" ниже.
# В Debug тоже -O2 (см. PURRMIDI_FAST_DEBUG ниже): иначе движок Lead в Debug выходит за бюджет блока.
target_compile_options(DaisySP PRIVATE $<$<OR:$<CONFIG:Release>,$<BOOL:${PURRMIDI_FAST_DEBUG}>>:-O2>)

# ---------------------------------------------------------------------------
# Application sources
# ---------------------------------------------------------------------------

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Звуковые движки (все входят в прошивку, выбор — по MIDI Program Change)
    Engine/synth/synth_engine.cpp
    Engine/synth/synth_arena.cpp
    Engine/synth/epiano_synth.cpp
    Engine/synth/lead_synth.cpp
    Engine/synth/organ_synth.cpp
    Engine/synth/pluck_synth.cpp
    Engine/synth/soundfont_synth.cpp
    Engine/synth/tinysoundfont_impl.cpp
    Engine/synth/violin_synth.cpp

    Middlewares/Third_Party/bfreeOrgan2/synth_organ.cpp
    Middlewares/Third_Party/bfreeOrgan2/components/voice_organ.cpp
    Middlewares/Third_Party/bfreeOrgan2/components/efx_chorus.cpp
    Middlewares/Third_Party/bfreeOrgan2/components/env_ar.cpp
    Middlewares/Third_Party/bfreeOrgan2/components/env_click.cpp

    Platform/midi_usb/usbh_midi.c
    Platform/display/display.c
    Platform/system/diag.c
    Platform/system/watchdog.c
    Platform/system/backup.c
    Platform/system/ram_init.c
    Engine/midi/chord_detector.c
    Engine/midi/midi_queue.c
    Engine/midi/midi_dispatch.c
    Engine/midi/usb_midi_packet.c
    Engine/audio/audio_halves.c
    Platform/midi_usb/midi_usb.c
    Platform/midi_usb/usb_port_recover.c

    Platform/display/st7735/st7735.c
    Platform/display/st7735/st7735_reg.c
)

# ---------------------------------------------------------------------------
# Оптимизация по скорости для горячего аудио-кода
# ---------------------------------------------------------------------------
# Весь проект собирается с -Os (размер). Для кода, который выполняется в аудио-прерывании на каждом
# блоке/сэмпле, важнее скорость. Измерено на Cortex-M7 (число инструкций на блок из 128 кадров):
#   орган, 12 голосов: -Os 172 тыс. -> -O2 131 тыс. (-24%);  epiano, 8 голосов: 111 тыс. -> 91 тыс. (-18%);
#   -O3 почти ничего не добавляет к -O2, но раздувает код (epiano +2,2 КБ).
# Только Release: Debug остаётся -O0 -g3. Опция -O2 попадает в командную строку после -Os и перекрывает её.
# Не включать сюда файлы, привязанные к ITCM по имени объекта в линкер-скрипте, при переходе на LTO:
# LTO меняет имена объектных файлов (см. секцию .itcm_text).
set(PURRMIDI_AUDIO_HOT_SOURCES
    Engine/synth/synth_engine.cpp
    Engine/synth/epiano_synth.cpp
    Engine/synth/lead_synth.cpp
    Engine/synth/organ_synth.cpp
    Engine/synth/pluck_synth.cpp
    Engine/synth/soundfont_synth.cpp
    Engine/synth/tinysoundfont_impl.cpp
    Engine/synth/violin_synth.cpp

    Middlewares/Third_Party/bfreeOrgan2/synth_organ.cpp
    Middlewares/Third_Party/bfreeOrgan2/components/voice_organ.cpp
    Middlewares/Third_Party/bfreeOrgan2/components/efx_chorus.cpp
    Middlewares/Third_Party/bfreeOrgan2/components/env_ar.cpp
    Middlewares/Third_Party/bfreeOrgan2/components/env_click.cpp
)
# Debug (-O0) делает аудио-код в 3-5 раз медленнее: на Cortex-M7 один голос SoundFont в Debug стоит ~100 мкс на блок
# из 128 кадров (-O2 - в разы меньше), поэтому горячие файлы по умолчанию собираются с -O2 и в Debug
# (отладочная информация -g3 остаётся). Отключить: -DPURRMIDI_HOT_OPT_IN_DEBUG=OFF.
option(PURRMIDI_HOT_OPT_IN_DEBUG "Compile audio hot sources with -O2 also in Debug builds" ON)
# -fno-math-errno: sqrtf/sqrt становятся одной инструкцией VSQRT вместо вызова libm с обработкой errno.
# Ничего не меняет в результатах (аргументы вне области определения по-прежнему дают NaN), errno нигде не читается.
if(PURRMIDI_HOT_OPT_IN_DEBUG)
    set(PURRMIDI_HOT_OPT "-O2;-fno-math-errno")
else()
    set(PURRMIDI_HOT_OPT "$<$<CONFIG:Release>:-O2;-fno-math-errno>")
endif()
set_source_files_properties(${PURRMIDI_AUDIO_HOT_SOURCES}
    Platform/qspi_xip/pm_crc32.c
    PROPERTIES COMPILE_OPTIONS "${PURRMIDI_HOT_OPT}")

# ---------------------------------------------------------------------------
# SoundFont: банк хранится ТОЛЬКО в QSPI Flash и читается на месте (XIP, 0x90000000)
# ---------------------------------------------------------------------------
# SD-карта (SDMMC1 + FatFs) нужна только при запуске: если на карте лежит новый 0:/SNDFNT.SF2, загрузчик
# перезаписывает им банк во флеше (Platform/qspi_xip/sf2_loader.c), после чего SD отключается.
# Подробности: docs/SOUNDFONT.md.
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Platform/sd/sd_storage.c
    Platform/qspi_xip/pm_crc32.c
    Platform/qspi_xip/sf2_loader_core.c
    Platform/qspi_xip/sf2_loader.c
    Platform/qspi_xip/qspi_flash.c
    Platform/qspi_xip/xip_mpu.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
    Platform/display
    Platform/system
    Platform/sd
    Engine/midi
    Engine/audio
    Engine/synth
    Platform/midi_usb
    Platform/qspi_xip
    Platform/display/st7735
)
target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM PRIVATE
    Middlewares/Third_Party/bfreeOrgan2
    Middlewares/Third_Party/bfreeOrgan2/components
    Middlewares/Third_Party/TinySoundFont
)
# -Wextra только для кода проекта: Engine/, Platform/ и Core/Src/main.c. Сгенерированные CubeMX файлы (user_diskio.c,
# usbh_conf.c и т. п.) и сторонние каталоги остаются на -Wall: -Wextra даёт там шум (неиспользуемые параметры заглушек),
# который CubeMX возвращает при каждой регенерации. Хост-тесты уже собираются с -Wextra.
get_target_property(_purrmidi_own_sources ${CMAKE_PROJECT_NAME} SOURCES)
list(FILTER _purrmidi_own_sources INCLUDE REGEX "^(Engine|Platform)/")
list(APPEND _purrmidi_own_sources "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/main.c")
# APPEND обязателен: у горячих аудио-файлов уже задан COMPILE_OPTIONS (-O2, см. выше), а обычное присваивание
# молча заменило бы его, и они собрались бы с -Os.
set_property(SOURCE ${_purrmidi_own_sources} APPEND PROPERTY COMPILE_OPTIONS "-Wextra")

if(PURRMIDI_WERROR)
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -Werror)
endif()

# Remove an incorrect legacy library dependency when linking C++.
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Простой синтаксис (без PRIVATE) для совместимости со скриптом CubeMX
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    DaisySP
)

# ---------------------------------------------------------------------------
# Debug: быстрый рантайм-путь (прерывания, USB, HAL, DaisySP) при сохранении -O0 для остального кода
# ---------------------------------------------------------------------------
if(PURRMIDI_FAST_DEBUG)
    foreach(_t STM32_Drivers USB_Host_Library)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE $<$<CONFIG:Debug>:-O2>)
        endif()
    endforeach()
    set(_purrmidi_fast_debug_sources
        Engine/audio/audio_halves.c
        Engine/midi/midi_queue.c
        Engine/midi/midi_dispatch.c
        Engine/midi/usb_midi_packet.c
        Engine/midi/chord_detector.c
        Platform/midi_usb/usbh_midi.c
        Platform/midi_usb/midi_usb.c
        Platform/midi_usb/usb_port_recover.c
        Platform/system/diag.c
        Platform/system/watchdog.c
        Platform/qspi_xip/xip_mpu.c
        Platform/qspi_xip/qspi_flash.c
        Core/Src/stm32h7xx_it.c
        Core/Src/dma.c
        Core/Src/sai.c
    )
    set_property(SOURCE ${_purrmidi_fast_debug_sources} APPEND PROPERTY COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()

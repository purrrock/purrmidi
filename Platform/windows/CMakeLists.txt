cmake_minimum_required(VERSION 3.22)

if(WIN32)
    option(PURRMIDI_BUILD_APP "Build Windows executable purrmidi_win" ON)
else()
    option(PURRMIDI_BUILD_APP "Build Windows executable purrmidi_win" OFF)
endif()

option(PURRMIDI_BUILD_TESTS "Build host unit tests" ON)
# Предупреждения GCC/Clang в коде проекта считаются ошибками (в CI включено). Сторонние каталоги
# (DaisySP, bfreeOrgan2, TinySoundFont) подключаются как SYSTEM.
option(PURRMIDI_WERROR "Treat compiler warnings in project code as errors (GCC/Clang)" OFF)

# Все синтезаторы (epiano / pluck / lead / organ / violin / soundfont) входят в сборку; активный
# выбирается во время работы сообщением MIDI Program Change (см. Engine/synth/synth_engine.h).
# Урезанная копия DaisySP (Oscillator, Svf, Adsr) лежит прямо в репозитории; подмодулей git в проекте нет.
set(DAISYSP_DIR "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/DaisySP_lite")
if(NOT EXISTS "${DAISYSP_DIR}/Source")
    message(FATAL_ERROR
        "Исходники DaisySP не найдены: ${DAISYSP_DIR}/Source\n"
        "(они нужны синтезатору lead, а он всегда входит в сборку; проверьте, что репозиторий склонирован полностью)"
    )
endif()

# ---------------------------------------------------------------------------
# Синтезаторы: каждый — отдельная статическая библиотека со своим API
# ---------------------------------------------------------------------------

# common: общая арена памяти голосов (synth_arena.h); synth_common.h - только заголовок
add_library(purrmidi_synth_common STATIC
    "${CMAKE_SOURCE_DIR}/Engine/synth/synth_arena.cpp"
)
target_include_directories(purrmidi_synth_common PUBLIC
    "${CMAKE_SOURCE_DIR}/Engine/synth"
)
target_compile_features(purrmidi_synth_common PUBLIC cxx_std_17)

# epiano: полифоническое FM-электропиано (без внешних зависимостей)
add_library(purrmidi_synth_epiano STATIC
    "${CMAKE_SOURCE_DIR}/Engine/synth/epiano_synth.cpp"
)
target_include_directories(purrmidi_synth_epiano PUBLIC
    "${CMAKE_SOURCE_DIR}/Engine/synth"
)
target_compile_features(purrmidi_synth_epiano PUBLIC cxx_std_17)

# organ: тонколесный орган (bfreeOrgan2)
add_library(purrmidi_synth_organ STATIC
    "${CMAKE_SOURCE_DIR}/Engine/synth/organ_synth.cpp"
    "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/bfreeOrgan2/synth_organ.cpp"
    "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/bfreeOrgan2/components/voice_organ.cpp"
    "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/bfreeOrgan2/components/efx_chorus.cpp"
    "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/bfreeOrgan2/components/env_ar.cpp"
    "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/bfreeOrgan2/components/env_click.cpp"
)
target_include_directories(purrmidi_synth_organ PUBLIC
    "${CMAKE_SOURCE_DIR}/Engine/synth"
)
target_include_directories(purrmidi_synth_organ SYSTEM PUBLIC
    "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/bfreeOrgan2"
    "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/bfreeOrgan2/components"
)
target_compile_features(purrmidi_synth_organ PUBLIC cxx_std_17)

# pluck: нейлоновая струна, собственная модель Карплуса-Стронга (без внешних зависимостей)
add_library(purrmidi_synth_pluck STATIC
    "${CMAKE_SOURCE_DIR}/Engine/synth/pluck_synth.cpp"
)
target_include_directories(purrmidi_synth_pluck PUBLIC
    "${CMAKE_SOURCE_DIR}/Engine/synth"
)
target_compile_features(purrmidi_synth_pluck PUBLIC cxx_std_17)
target_link_libraries(purrmidi_synth_pluck PUBLIC purrmidi_synth_common)

# lead: полифонический субтрактивный лид-синтезатор на DaisySP (Oscillator, Svf, Adsr)
add_library(purrmidi_synth_lead STATIC
    "${CMAKE_SOURCE_DIR}/Engine/synth/lead_synth.cpp"
    "${DAISYSP_DIR}/Source/Synthesis/oscillator.cpp"
    "${DAISYSP_DIR}/Source/Filters/svf.cpp"
    "${DAISYSP_DIR}/Source/Control/adsr.cpp"
)
target_include_directories(purrmidi_synth_lead PUBLIC
    "${CMAKE_SOURCE_DIR}/Engine/synth"
)
target_include_directories(purrmidi_synth_lead SYSTEM PUBLIC
    "${DAISYSP_DIR}/Source"
    "${DAISYSP_DIR}/Source/Utility"
    "${DAISYSP_DIR}/Source/Synthesis"
    "${DAISYSP_DIR}/Source/Filters"
    "${DAISYSP_DIR}/Source/Control"
)
target_compile_features(purrmidi_synth_lead PUBLIC cxx_std_17)
target_link_libraries(purrmidi_synth_lead PUBLIC purrmidi_synth_common)

# soundfont: синтезатор SF2 на базе TinySoundFont; банк читается из ОЗУ (хост-аналог QSPI XIP)
add_library(purrmidi_synth_soundfont STATIC
    "${CMAKE_SOURCE_DIR}/Engine/synth/soundfont_synth.cpp"
    "${CMAKE_SOURCE_DIR}/Platform/host/sf2_loader_host.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/synth/tinysoundfont_impl.cpp"
)
target_include_directories(purrmidi_synth_soundfont PUBLIC
    "${CMAKE_SOURCE_DIR}/Engine/synth"
    "${CMAKE_SOURCE_DIR}/Platform/host"
    "${CMAKE_SOURCE_DIR}/Platform/qspi_xip"
)
target_include_directories(purrmidi_synth_soundfont SYSTEM PUBLIC
    "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/TinySoundFont"
)
target_compile_features(purrmidi_synth_soundfont PUBLIC cxx_std_17)

# violin: полифоническая скрипка - волновод смычковой струны и резонансы деки (без внешних зависимостей)
add_library(purrmidi_synth_violin STATIC
    "${CMAKE_SOURCE_DIR}/Engine/synth/violin_synth.cpp"
)
target_include_directories(purrmidi_synth_violin PUBLIC
    "${CMAKE_SOURCE_DIR}/Engine/synth"
)
target_compile_features(purrmidi_synth_violin PUBLIC cxx_std_17)
target_link_libraries(purrmidi_synth_violin PUBLIC purrmidi_synth_common)

foreach(_synth pluck lead epiano organ soundfont violin)
    if(MSVC)
        target_compile_options(purrmidi_synth_${_synth} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(purrmidi_synth_${_synth} PRIVATE -Wall -Wextra)
        if(PURRMIDI_WERROR)
            target_compile_options(purrmidi_synth_${_synth} PRIVATE -Werror)
            # Исходники сторонних библиотек (Middlewares/) собираются в наших целях: их предупреждения не ошибки.
            get_target_property(_synth_sources purrmidi_synth_${_synth} SOURCES)
            foreach(_src IN LISTS _synth_sources)
                if(_src MATCHES "/Middlewares/")
                    set_source_files_properties("${_src}" PROPERTIES COMPILE_OPTIONS "-Wno-error")
                endif()
            endforeach()
        endif()
    endif()
    if(UNIX)
        target_link_libraries(purrmidi_synth_${_synth} PUBLIC m)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# purrmidi_core: MIDI-очередь + диспетчер + рантайм-переключатель синтезаторов
# ---------------------------------------------------------------------------
add_library(purrmidi_core STATIC
    "${CMAKE_SOURCE_DIR}/Engine/synth/synth_engine.cpp"
    "${CMAKE_SOURCE_DIR}/Engine/midi/chord_detector.c"
    "${CMAKE_SOURCE_DIR}/Engine/midi/midi_queue.c"
    "${CMAKE_SOURCE_DIR}/Engine/midi/midi_dispatch.c"
)

target_include_directories(purrmidi_core PUBLIC
    "${CMAKE_SOURCE_DIR}/Engine/midi"
    "${CMAKE_SOURCE_DIR}/Engine/synth"
)

target_link_libraries(purrmidi_core PUBLIC
    purrmidi_synth_epiano
    purrmidi_synth_lead
    purrmidi_synth_organ
    purrmidi_synth_pluck
    purrmidi_synth_soundfont
    purrmidi_synth_violin
)
target_compile_features(purrmidi_core PUBLIC c_std_11 cxx_std_17)

if(MSVC)
    target_compile_options(purrmidi_core PRIVATE /utf-8 /W4)
else()
    target_compile_options(purrmidi_core PRIVATE -Wall -Wextra)
    if(PURRMIDI_WERROR)
        target_compile_options(purrmidi_core PRIVATE -Werror)
    endif()
endif()

if(PURRMIDI_BUILD_APP)
    include(FetchContent)

    # miniaudio
    FetchContent_Declare(
        miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG        0.11.21
    )
    FetchContent_MakeAvailable(miniaudio)

    # RtMidi
    set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        RtMidi
        GIT_REPOSITORY https://github.com/thestk/rtmidi.git
        GIT_TAG        6.0.0
    )
    FetchContent_MakeAvailable(RtMidi)

    add_executable(purrmidi_win
        main_win.cpp
        midi_decode.cpp
        midi_input.cpp
        audio_out.cpp
        wav_recorder.cpp
        virtual_keyboard.cpp
    )

    set_target_properties(purrmidi_win PROPERTIES
        OUTPUT_NAME "purrmidi_win"
        C_STANDARD 11
        CXX_STANDARD 17
    )

    target_include_directories(purrmidi_win PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${miniaudio_SOURCE_DIR}
    )

    target_link_libraries(purrmidi_win PRIVATE
        purrmidi_core
        rtmidi
    )

    if(WIN32)
        target_link_libraries(purrmidi_win PRIVATE winmm ksuser)
    endif()

    if(MINGW)
        target_link_options(purrmidi_win PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    if(MSVC)
        target_compile_options(purrmidi_win PRIVATE /utf-8 /W4)
    else()
        target_compile_options(purrmidi_win PRIVATE -Wall -Wextra)
    endif()
endif()

if(PURRMIDI_BUILD_TESTS)
    add_subdirectory("${CMAKE_SOURCE_DIR}/tests" "${CMAKE_BINARY_DIR}/tests")
endif()

cmake_minimum_required(VERSION 3.22)

if(WIN32)
    option(PURRMIDI_BUILD_APP "Build Windows executable purrmidi_win" ON)
else()
    option(PURRMIDI_BUILD_APP "Build Windows executable purrmidi_win" OFF)
endif()

option(PURRMIDI_BUILD_TESTS "Build host unit tests" ON)

# Check DaisySP submodule
set(DAISYSP_DIR "${CMAKE_SOURCE_DIR}/Middlewares/Third_Party/DaisySP")
if(NOT EXISTS "${DAISYSP_DIR}/Source")
    message(FATAL_ERROR
        "Исходники DaisySP не найдены: ${DAISYSP_DIR}/Source\n"
        "Выполните команду для инициализации подмодулей:\n"
        "git submodule update --init --recursive"
    )
endif()

# purrmidi_core static library
add_library(purrmidi_core STATIC
    "${CMAKE_SOURCE_DIR}/Core/Src/pluck_synth.cpp"
    "${CMAKE_SOURCE_DIR}/Core/Src/chord_detector.c"
    "${CMAKE_SOURCE_DIR}/Engine/midi/midi_queue.c"
    "${CMAKE_SOURCE_DIR}/Engine/midi/midi_dispatch.c"
    "${DAISYSP_DIR}/DaisySP-LGPL/Source/PhysicalModeling/pluck.cpp"
)

target_include_directories(purrmidi_core PUBLIC
    "${CMAKE_SOURCE_DIR}/Core/Inc"
    "${CMAKE_SOURCE_DIR}/Engine/midi"
    "${DAISYSP_DIR}/Source"
    "${DAISYSP_DIR}/DaisySP-LGPL/Source"
)

target_compile_definitions(purrmidi_core PUBLIC USE_DAISYSP_LGPL)
target_compile_features(purrmidi_core PUBLIC c_std_11 cxx_std_17)

if(MSVC)
    target_compile_options(purrmidi_core PRIVATE /utf-8 /W4)
else()
    target_compile_options(purrmidi_core PRIVATE -Wall -Wextra)
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

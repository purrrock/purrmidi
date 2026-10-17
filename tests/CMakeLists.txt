cmake_minimum_required(VERSION 3.22)

# Часть тестов использует голый assert(): при Release/RelWithDebInfo (-DNDEBUG) проверки исчезли бы,
# и тесты «проходили» бы вхолостую. Для тестов NDEBUG всегда отключаем.
if(MSVC)
    add_compile_options(/UNDEBUG)
else()
    add_compile_options(-UNDEBUG)
endif()

add_executable(test_audio_halves
    test_audio_halves.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Engine/audio/audio_halves.c"
)
target_include_directories(test_audio_halves PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../Engine/audio"
)
target_compile_features(test_audio_halves PRIVATE cxx_std_17)
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(test_audio_halves PRIVATE Threads::Threads)
endif()
add_test(NAME test_audio_halves COMMAND test_audio_halves)

add_executable(test_usb_midi_packet
    test_usb_midi_packet.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Engine/midi/usb_midi_packet.c"
)
target_include_directories(test_usb_midi_packet PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../Engine/midi"
)
target_compile_features(test_usb_midi_packet PRIVATE cxx_std_17)
add_test(NAME test_usb_midi_packet COMMAND test_usb_midi_packet)

add_executable(test_midi_decode
    test_midi_decode.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/windows/midi_decode.cpp"
)
target_include_directories(test_midi_decode PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/windows"
)
target_compile_features(test_midi_decode PRIVATE cxx_std_17)
add_test(NAME test_midi_decode COMMAND test_midi_decode)

# Тесты отдельных синтезаторов линкуются с их собственными библиотеками.
add_executable(test_pluck_host
    test_pluck_host.cpp
)
target_link_libraries(test_pluck_host PRIVATE purrmidi_synth_pluck)
target_compile_features(test_pluck_host PRIVATE cxx_std_17)
add_test(NAME test_pluck_host COMMAND test_pluck_host)

add_executable(test_lead_host
    test_lead_host.cpp
)
target_link_libraries(test_lead_host PRIVATE purrmidi_synth_lead)
target_compile_features(test_lead_host PRIVATE cxx_std_17)
add_test(NAME test_lead_host COMMAND test_lead_host)

add_executable(test_sine_host
    test_sine_host.cpp
)
target_link_libraries(test_sine_host PRIVATE purrmidi_synth_sine)
target_compile_features(test_sine_host PRIVATE cxx_std_17)
add_test(NAME test_sine_host COMMAND test_sine_host)

add_executable(test_epiano_host
    test_epiano_host.cpp
)
target_link_libraries(test_epiano_host PRIVATE purrmidi_synth_epiano)
target_compile_features(test_epiano_host PRIVATE cxx_std_17)
add_test(NAME test_epiano_host COMMAND test_epiano_host)

add_executable(test_organ_host
    test_organ_host.cpp
)
target_link_libraries(test_organ_host PRIVATE purrmidi_synth_organ)
target_compile_features(test_organ_host PRIVATE cxx_std_17)
add_test(NAME test_organ_host COMMAND test_organ_host)

add_executable(test_soundfont_host
    test_soundfont_host.cpp
)
target_include_directories(test_soundfont_host PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../Core/Inc"
    "${CMAKE_CURRENT_LIST_DIR}/../Middlewares/Third_Party/TinySoundFont"
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/host"
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/qspi_xip"
)
target_link_libraries(test_soundfont_host PRIVATE purrmidi_synth_soundfont purrmidi_core)
target_compile_features(test_soundfont_host PRIVATE cxx_std_17)
# Работает из корня репозитория: хост-загрузчик (Platform/host) ищет tests/SNDFNT.SF2 относительно
# рабочего каталога. Иначе тест молча пропускает все проверки с реальным SoundFont.
add_test(NAME test_soundfont_host COMMAND test_soundfont_host
         WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/..")

add_executable(test_dulcimer_host
    test_dulcimer_host.cpp
)
target_link_libraries(test_dulcimer_host PRIVATE purrmidi_synth_dulcimer)
target_compile_features(test_dulcimer_host PRIVATE cxx_std_17)
add_test(NAME test_dulcimer_host COMMAND test_dulcimer_host)

add_executable(test_flute_host
    test_flute_host.cpp
)
target_link_libraries(test_flute_host PRIVATE purrmidi_synth_flute)
target_compile_features(test_flute_host PRIVATE cxx_std_17)
add_test(NAME test_flute_host COMMAND test_flute_host)

add_executable(test_saxophone_host
    test_saxophone_host.cpp
)
target_link_libraries(test_saxophone_host PRIVATE purrmidi_synth_saxophone purrmidi_core)
target_compile_features(test_saxophone_host PRIVATE cxx_std_17)
add_test(NAME test_saxophone_host COMMAND test_saxophone_host)

add_executable(test_queue_dispatch
    test_queue_dispatch.cpp
)
target_link_libraries(test_queue_dispatch PRIVATE purrmidi_core)
target_compile_features(test_queue_dispatch PRIVATE cxx_std_17)
add_test(NAME test_queue_dispatch COMMAND test_queue_dispatch)

add_executable(test_synth_switch
    test_synth_switch.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(test_synth_switch PRIVATE purrmidi_core Threads::Threads)
target_compile_features(test_synth_switch PRIVATE cxx_std_17)
add_test(NAME test_synth_switch COMMAND test_synth_switch)

add_executable(test_synth_common
    test_synth_common.cpp
)
target_link_libraries(test_synth_common PRIVATE purrmidi_synth_common)
target_compile_features(test_synth_common PRIVATE cxx_std_17)
add_test(NAME test_synth_common COMMAND test_synth_common)

add_executable(test_engine_golden
    test_engine_golden.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(test_engine_golden PRIVATE purrmidi_core Threads::Threads)
target_compile_features(test_engine_golden PRIVATE cxx_std_17)
add_test(NAME test_engine_golden COMMAND test_engine_golden)

add_executable(test_chord_detector
    test_chord_detector.cpp
)
target_link_libraries(test_chord_detector PRIVATE purrmidi_core)
target_compile_features(test_chord_detector PRIVATE cxx_std_17)
add_test(NAME test_chord_detector COMMAND test_chord_detector)

add_executable(test_virtual_keyboard
    test_virtual_keyboard.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/windows/virtual_keyboard.cpp"
)
target_include_directories(test_virtual_keyboard PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../Core/Inc"
    "${CMAKE_CURRENT_LIST_DIR}/../Engine/midi"
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/windows"
)
target_compile_features(test_virtual_keyboard PRIVATE cxx_std_17)
add_test(NAME test_virtual_keyboard COMMAND test_virtual_keyboard)

# ---------------------------------------------------------------------------
# QSPI XIP: загрузчик SoundFont-банка во флеш (логика без HAL) и синтезатор поверх «флеша» в ОЗУ
# ---------------------------------------------------------------------------
# Ядро загрузчика проверяется на эмуляции NOR-флеша (в том числе сбой питания посреди записи).
add_executable(test_sf2_loader_core
    test_sf2_loader_core.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/qspi_xip/sf2_loader_core.c"
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/qspi_xip/pm_crc32.c"
)
target_include_directories(test_sf2_loader_core PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/qspi_xip"
)
target_compile_features(test_sf2_loader_core PRIVATE cxx_std_17)
add_test(NAME test_sf2_loader_core COMMAND test_sf2_loader_core)

# Синтезатор SoundFont + настоящий TinySoundFont; «флеш» - файл банка в памяти (Platform/host).
add_executable(test_sf2_xip_host
    test_sf2_xip_host.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../Engine/synth/soundfont_synth.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/host/sf2_loader_host.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../Engine/synth/tinysoundfont_impl.cpp"
)
target_include_directories(test_sf2_xip_host PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../Core/Inc"
    "${CMAKE_CURRENT_LIST_DIR}/../Engine/synth"
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/qspi_xip"
    "${CMAKE_CURRENT_LIST_DIR}/../Platform/host"
    "${CMAKE_CURRENT_LIST_DIR}/../Middlewares/Third_Party/TinySoundFont"
)
target_compile_features(test_sf2_xip_host PRIVATE cxx_std_17)
if(UNIX)
    target_link_libraries(test_sf2_xip_host PRIVATE m)
endif()
add_test(NAME test_sf2_xip_host COMMAND test_sf2_xip_host
         WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/..")

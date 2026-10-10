cmake_minimum_required(VERSION 3.22)

add_executable(test_midi_decode
    test_midi_decode.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../platform/windows/midi_decode.cpp"
)
target_include_directories(test_midi_decode PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../platform/windows"
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
)
target_link_libraries(test_soundfont_host PRIVATE purrmidi_synth_soundfont purrmidi_core)
target_compile_features(test_soundfont_host PRIVATE cxx_std_17)
# Работает из корня репозитория: хост-вариант SF2Cache ищет tests/SNDFNT.SF2 относительно
# рабочего каталога. Иначе тест молча пропускает все проверки с реальным SoundFont.
add_test(NAME test_soundfont_host COMMAND test_soundfont_host
         WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/..")

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

add_executable(test_chord_detector
    test_chord_detector.cpp
)
target_link_libraries(test_chord_detector PRIVATE purrmidi_core)
target_compile_features(test_chord_detector PRIVATE cxx_std_17)
add_test(NAME test_chord_detector COMMAND test_chord_detector)

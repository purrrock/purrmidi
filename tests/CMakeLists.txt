cmake_minimum_required(VERSION 3.22)

add_executable(test_midi_decode
    test_midi_decode.cpp
    "${CMAKE_SOURCE_DIR}/platform/windows/midi_decode.cpp"
)
target_include_directories(test_midi_decode PRIVATE
    "${CMAKE_SOURCE_DIR}/platform/windows"
)
target_compile_features(test_midi_decode PRIVATE cxx_std_17)
add_test(NAME test_midi_decode COMMAND test_midi_decode)

add_executable(test_pluck_host
    test_pluck_host.cpp
)
target_link_libraries(test_pluck_host PRIVATE purrmidi_core)
target_compile_features(test_pluck_host PRIVATE cxx_std_17)
add_test(NAME test_pluck_host COMMAND test_pluck_host)

add_executable(test_queue_dispatch
    test_queue_dispatch.cpp
)
target_link_libraries(test_queue_dispatch PRIVATE purrmidi_core)
target_compile_features(test_queue_dispatch PRIVATE cxx_std_17)
add_test(NAME test_queue_dispatch COMMAND test_queue_dispatch)

add_executable(test_chord_detector
    test_chord_detector.cpp
)
target_link_libraries(test_chord_detector PRIVATE purrmidi_core)
target_compile_features(test_chord_detector PRIVATE cxx_std_17)
add_test(NAME test_chord_detector COMMAND test_chord_detector)

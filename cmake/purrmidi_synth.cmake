# ---------------------------------------------------------------------------
# Выбор звукового движка на этапе компиляции (общий для прошивки STM32
# и Windows/host-сборки).
#
#   cmake ... -DPURRMIDI_SYNTH=sine      одноголосая синусоида (проверка тракта)
#   cmake ... -DPURRMIDI_SYNTH=pluck     Карплус-Стронг, DaisySP Pluck (по умолчанию)
#   cmake ... -DPURRMIDI_SYNTH=epiano    полифоническое FM-электропиано
#
# После include() доступны переменные:
#   PURRMIDI_SYNTH_NAME          нормализованное имя (sine|pluck|epiano)
#   PURRMIDI_SYNTH_DEFINE        макрос для компилятора (PURRMIDI_SYNTH_SINE и т.д.)
#   PURRMIDI_SYNTH_SOURCES       исходники выбранного синтезатора (от корня проекта)
#   PURRMIDI_SYNTH_NEEDS_DAISYSP ON, если выбранному движку нужна библиотека DaisySP
# ---------------------------------------------------------------------------

set(PURRMIDI_SYNTH "pluck" CACHE STRING "Synth engine: sine, pluck or epiano")
set_property(CACHE PURRMIDI_SYNTH PROPERTY STRINGS sine pluck epiano)

string(TOLOWER "${PURRMIDI_SYNTH}" PURRMIDI_SYNTH_NAME)

if(PURRMIDI_SYNTH_NAME STREQUAL "sine")
    set(PURRMIDI_SYNTH_DEFINE PURRMIDI_SYNTH_SINE)
    set(PURRMIDI_SYNTH_SOURCES Core/Src/synth.c Core/Src/sine_synth.c)
    set(PURRMIDI_SYNTH_NEEDS_DAISYSP OFF)
elseif(PURRMIDI_SYNTH_NAME STREQUAL "pluck")
    set(PURRMIDI_SYNTH_DEFINE PURRMIDI_SYNTH_PLUCK)
    set(PURRMIDI_SYNTH_SOURCES Core/Src/pluck_synth.cpp)
    set(PURRMIDI_SYNTH_NEEDS_DAISYSP ON)
elseif(PURRMIDI_SYNTH_NAME STREQUAL "epiano")
    set(PURRMIDI_SYNTH_DEFINE PURRMIDI_SYNTH_EPIANO)
    set(PURRMIDI_SYNTH_SOURCES Core/Src/epiano_synth.cpp)
    set(PURRMIDI_SYNTH_NEEDS_DAISYSP OFF)
else()
    message(FATAL_ERROR
        "Неизвестный синтезатор PURRMIDI_SYNTH='${PURRMIDI_SYNTH}'. "
        "Допустимые значения: sine, pluck, epiano")
endif()

message(STATUS "PurrMidi synth engine: ${PURRMIDI_SYNTH_NAME}")

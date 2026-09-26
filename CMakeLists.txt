cmake_minimum_required(VERSION 3.22)

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

# Main firmware executable
add_executable(${CMAKE_PROJECT_NAME})

# STM32CubeMX-generated sources and libraries
add_subdirectory(cmake/stm32cubemx)

# ---------------------------------------------------------------------------
# DaisySP
# ---------------------------------------------------------------------------

set(DAISYSP_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/DaisySP"
)

if(NOT EXISTS "${DAISYSP_DIR}/Source")
    message(FATAL_ERROR
        "DaisySP source directory not found: ${DAISYSP_DIR}/Source\n"
        "Initialize the DaisySP git submodule:\n"
        "git submodule update --init --recursive"
    )
endif()

# Compile DaisySP C++ sources directly into a static library.
file(GLOB_RECURSE DAISYSP_SOURCES CONFIGURE_DEPENDS
    "${DAISYSP_DIR}/Source/*.cpp"
    "${DAISYSP_DIR}/DaisySP-LGPL/Source/*.cpp"
)

if(NOT DAISYSP_SOURCES)
    message(FATAL_ERROR
        "No DaisySP C++ source files found in ${DAISYSP_DIR}/Source"
    )
endif()

add_library(DaisySP STATIC
    ${DAISYSP_SOURCES}
)

# Передаем аппаратные флаги микроконтроллера (-mcpu, -mthumb) из CubeMX в DaisySP
target_link_libraries(DaisySP PRIVATE stm32cubemx)

# Add the DaisySP source directory and all its subdirectories
# to the include search path.
file(GLOB_RECURSE DAISYSP_HEADERS CONFIGURE_DEPENDS
    "${DAISYSP_DIR}/Source/*.h"
    "${DAISYSP_DIR}/Source/*.hpp"
    "${DAISYSP_DIR}/DaisySP-LGPL/Source/*.h"
    "${DAISYSP_DIR}/DaisySP-LGPL/Source/*.hpp"
)

set(DAISYSP_INCLUDE_DIRS
    "${DAISYSP_DIR}/Source"
    "${DAISYSP_DIR}/DaisySP-LGPL/Source"
)

foreach(HEADER ${DAISYSP_HEADERS})
    get_filename_component(HEADER_DIR "${HEADER}" DIRECTORY)
    list(APPEND DAISYSP_INCLUDE_DIRS "${HEADER_DIR}")
endforeach()

list(REMOVE_DUPLICATES DAISYSP_INCLUDE_DIRS)

target_include_directories(DaisySP PUBLIC
    ${DAISYSP_INCLUDE_DIRS}
)

target_compile_definitions(DaisySP PUBLIC USE_DAISYSP_LGPL)

target_compile_features(DaisySP PUBLIC cxx_std_17)

# ---------------------------------------------------------------------------
# Application sources
# ---------------------------------------------------------------------------

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/synth.c
    Core/Src/pluck_synth.cpp
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
)

# Remove an incorrect legacy library dependency when linking C++.
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Простой синтаксис (без PRIVATE) для совместимости со скриптом CubeMX
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    DaisySP
)
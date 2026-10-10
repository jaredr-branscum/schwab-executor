cmake_minimum_required(VERSION 3.20)
project(schwab_executor VERSION 1.0.0 LANGUAGES CXX)

# Enforce Modern C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Modern compiler warning flags and multi-processor build flag
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Enable testing
enable_testing()

# Third-party dependencies via FetchContent
include(FetchContent)

# 1. Catch2 v3 for unit testing
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# Core library target
add_library(schwab_core
    src/process_runner.cpp
)

target_include_directories(schwab_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Subdirectories
add_subdirectory(tests)

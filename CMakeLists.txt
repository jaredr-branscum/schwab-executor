cmake_minimum_required(VERSION 3.20)
project(schwab_executor VERSION 1.0.0 LANGUAGES CXX)

# Enforce Modern C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Modern compiler warning flags and multi-processor build flag
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP /arch:AVX2)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -mavx2 -mfma)
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
# 2. nlohmann/json for JSON parsing
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2 nlohmann_json)

# Core library target
add_library(schwab_core
    src/process_runner.cpp
    src/token.cpp
    src/token_manager.cpp
    src/config.cpp
    src/indicators.cpp
    src/order.cpp
    src/risk_guard.cpp
    src/http_types.cpp
)

target_include_directories(schwab_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(schwab_core PUBLIC
    nlohmann_json::nlohmann_json
)

# Main CLI executable
add_executable(schwab_executor
    src/main.cpp
)

target_link_libraries(schwab_executor PRIVATE
    schwab_core
)

# Subdirectories
add_subdirectory(tests)

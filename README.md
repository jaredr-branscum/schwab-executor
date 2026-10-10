# Schwab Low-Latency C++ Execution Engine (`schwab-executor`)

High-performance, low-latency execution engine engineered in C++20 for Charles Schwab API trading accounts.

## Project Structure
```text
schwab-executor/
├── CMakeLists.txt          # Modern C++20 build definition (MSVC / Clang / GCC)
├── run_tests.bat           # 1-click test runner (Windows)
├── .gitignore              # Ignores build artifacts, tokens, and logs
├── include/                # Public C++ header files (.hpp)
│   └── schwab/
├── src/                    # C++ implementation files (.cpp)
├── tests/                  # Catch2 unit test suites
│   ├── CMakeLists.txt
│   └── test_sanity.cpp
└── python/                 # Self-contained Python helpers (OAuth login & reference status)
```

## Quick Start & Running Tests

### Prerequisites
- Windows 10/11
- Visual Studio Build Tools / MSVC 2022 (v143 / v18) with C++20 support
- CMake 3.20+

### Run Unit Tests
From the root directory, run:
```cmd
run_tests.bat
```
This script will:
1. Configure CMake with `/MP` (multi-processor compilation)
2. Build the project in Debug mode in parallel
3. Execute the Catch2 unit test suite with detailed console reporting

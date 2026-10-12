@echo off
setlocal

:: ==============================================================================
:: schwab-executor Build Script (Windows MSVC / CMake C++20)
:: Usage: build.bat [Release | Debug | all | clean | test]
:: ==============================================================================

set CONFIG=Release
set DO_CLEAN=0
set DO_TEST=0
set BUILD_ALL=0

:parse_args
if "%~1"=="" goto after_args
if /i "%~1"=="Release" (
    set CONFIG=Release
    shift
    goto parse_args
)
if /i "%~1"=="Debug" (
    set CONFIG=Debug
    shift
    goto parse_args
)
if /i "%~1"=="all" (
    set BUILD_ALL=1
    shift
    goto parse_args
)
if /i "%~1"=="--all" (
    set BUILD_ALL=1
    shift
    goto parse_args
)
if /i "%~1"=="clean" (
    set DO_CLEAN=1
    shift
    goto parse_args
)
if /i "%~1"=="--clean" (
    set DO_CLEAN=1
    shift
    goto parse_args
)
if /i "%~1"=="test" (
    set DO_TEST=1
    shift
    goto parse_args
)
if /i "%~1"=="--test" (
    set DO_TEST=1
    shift
    goto parse_args
)
if /i "%~1"=="help" goto usage
if /i "%~1"=="--help" goto usage
if /i "%~1"=="-h" goto usage
echo [WARNING] Unrecognized argument: %~1
shift
goto parse_args

:usage
echo ==============================================================================
echo   schwab-executor Build Utility
echo ==============================================================================
echo Usage: build.bat [configuration / option]
echo.
echo Options:
echo   Release        Build optimized Release configuration (default, recommended)
echo   Debug          Build Debug configuration with debug symbols
echo   all, --all     Build both Debug and Release configurations
echo   clean, --clean Clean build directory and rebuild fresh
echo   test, --test   Build and execute Catch2 unit test suite
echo   help, -h       Display this help message
echo.
exit /b 0

:after_args
:: Clean build directory if requested
if %DO_CLEAN% equ 1 (
    echo [CLEAN] Removing build directory...
    if exist "build" (
        rmdir /s /q "build"
        if %ERRORLEVEL% neq 0 (
            echo [ERROR] Failed to remove build directory.
            exit /b %ERRORLEVEL%
        )
    )
    echo [CLEAN] Build directory cleaned.
)

:: Locate CMake executable
set CMAKE_CMD=
where cmake >nul 2>nul
if %ERRORLEVEL% equ 0 set CMAKE_CMD=cmake

if not defined CMAKE_CMD if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set CMAKE_CMD="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not defined CMAKE_CMD if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set CMAKE_CMD="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not defined CMAKE_CMD if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set CMAKE_CMD="C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not defined CMAKE_CMD if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set CMAKE_CMD="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not defined CMAKE_CMD if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set CMAKE_CMD="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not defined CMAKE_CMD (
    echo [ERROR] CMake executable not found!
    echo Please install Visual Studio with C++ CMake tools or add cmake to your PATH.
    exit /b 1
)

:: Step 1: Configure CMake
echo [1/2] Configuring CMake build system...
%CMAKE_CMD% -B build -S .
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed.
    exit /b %ERRORLEVEL%
)

:: Step 2: Compile
if %BUILD_ALL% equ 1 (
    echo [2/2] Compiling C++ targets [Debug]...
    %CMAKE_CMD% --build build --config Debug --parallel
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] Debug compilation failed.
        exit /b %ERRORLEVEL%
    )
    echo [2/2] Compiling C++ targets [Release]...
    %CMAKE_CMD% --build build --config Release --parallel
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] Release compilation failed.
        exit /b %ERRORLEVEL%
    )
) else (
    echo [2/2] Compiling C++ targets [%CONFIG%]...
    %CMAKE_CMD% --build build --config %CONFIG% --parallel
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] Compilation failed for %CONFIG%.
        exit /b %ERRORLEVEL%
    )
)

:: Optional: Run test suite if requested
if %DO_TEST% equ 1 (
    echo [TEST] Running Catch2 unit test suite [%CONFIG%]...
    if exist "build\tests\%CONFIG%\run_unit_tests.exe" (
        build\tests\%CONFIG%\run_unit_tests.exe --reporter console
    ) else if exist "build\tests\Debug\run_unit_tests.exe" (
        build\tests\Debug\run_unit_tests.exe --reporter console
    ) else (
        %CMAKE_CMD% --build build --config %CONFIG% --target test
    )
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] One or more unit tests failed!
        exit /b %ERRORLEVEL%
    )
)

echo ==============================================================================
echo   BUILD COMPLETED SUCCESSFULLY!
if exist "build\Release\schwab_executor.exe" echo   * Release binary: build\Release\schwab_executor.exe
if exist "build\Debug\schwab_executor.exe"   echo   * Debug binary:   build\Debug\schwab_executor.exe
if exist "build\Release\schwab_core.lib"     echo   * Release lib:    build\Release\schwab_core.lib
if exist "build\Debug\schwab_core.lib"       echo   * Debug lib:      build\Debug\schwab_core.lib
echo ==============================================================================
exit /b 0

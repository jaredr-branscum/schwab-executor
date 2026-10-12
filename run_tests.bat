@echo off
setlocal

:: Locate cmake
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

set CONFIG=Debug
if /i "%~1"=="Release" set CONFIG=Release
if /i "%~1"=="Debug" set CONFIG=Debug

echo [1/3] Configuring CMake build system...
%CMAKE_CMD% -B build -S .
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed.
    exit /b %ERRORLEVEL%
)

echo [2/3] Compiling C++ targets [%CONFIG%]...
%CMAKE_CMD% --build build --config %CONFIG% --parallel
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Compilation failed.
    exit /b %ERRORLEVEL%
)

echo [3/3] Running Catch2 unit test suite...
if exist "build\tests\%CONFIG%\run_unit_tests.exe" (
    build\tests\%CONFIG%\run_unit_tests.exe --reporter console
) else (
    %CMAKE_CMD% --build build --config %CONFIG% --target test
)

if %ERRORLEVEL% neq 0 (
    echo [ERROR] One or more unit tests failed!
    exit /b %ERRORLEVEL%
)

echo ==============================================================================
echo   ALL UNIT TESTS PASSED SUCCESSFULLY!
echo ==============================================================================

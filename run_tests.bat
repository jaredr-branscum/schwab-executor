@echo off
setlocal

:: Locate cmake
where cmake >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set CMAKE_CMD=cmake
) else (
    set CMAKE_CMD="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

echo [1/3] Configuring CMake build system...
%CMAKE_CMD% -B build -S .
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed.
    exit /b %ERRORLEVEL%
)

echo [2/3] Compiling C++ targets (Debug)...
%CMAKE_CMD% --build build --config Debug --parallel
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Compilation failed.
    exit /b %ERRORLEVEL%
)

echo [3/3] Running Catch2 unit test suite...
if exist "build\tests\Debug\run_unit_tests.exe" (
    build\tests\Debug\run_unit_tests.exe --reporter console
) else (
    %CMAKE_CMD% --build build --config Debug --target test
)

if %ERRORLEVEL% neq 0 (
    echo [ERROR] One or more unit tests failed!
    exit /b %ERRORLEVEL%
)

echo ==============================================================================
echo   ALL UNIT TESTS PASSED SUCCESSFULLY!
echo ==============================================================================

@echo off
REM MistressDMC - DMC4SE MOOD : Windows build (Visual Studio / MSVC).
REM Requires CMake + Visual Studio (Desktop C++ workload).
REM Output: build\Release\dinput8.dll  (32-bit)

cmake -B build -A Win32
if errorlevel 1 goto :err
cmake --build build --config Release
if errorlevel 1 goto :err

echo.
echo Done: build\Release\dinput8.dll
goto :eof

:err
echo.
echo Build failed. Make sure CMake and Visual Studio (C++ desktop) are installed.
exit /b 1

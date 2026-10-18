@echo off
rem Сборка GUI AVR CLI Studio: main.cpp + ядро эмулятора (build/emu.cpp, EMU_LIB)
g++ -O2 -mwindows -std=c++17 -DEMU_LIB=1 -o "%~dp0avrstudio.exe" "%~dp0main.cpp" "%~dp0..\build\emu.cpp" ^
    -lcomctl32 -lcomdlg32 -lgdi32 -lshell32 -luser32
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK: %~dp0avrstudio.exe

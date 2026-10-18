@echo off
rem Сборка GUI-каркаса AVR CLI Studio (M1)
g++ -O2 -mwindows -std=c++17 -o "%~dp0avrstudio.exe" "%~dp0main.cpp" ^
    -lcomctl32 -lcomdlg32 -lgdi32 -lshell32 -luser32
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK: %~dp0avrstudio.exe

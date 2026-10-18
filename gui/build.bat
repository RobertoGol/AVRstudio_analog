@echo off
rem ============================================================
rem  AVR CLI Studio - GUI build (портативный один exe)
rem  - статическая линковка (ничего не нужно устанавливать)
rem  - 207 профилей чипов зашиты ресурсом res\portable.rc / procpack.bin
rem    (перегенерация pack: pack_procs.ps1)
rem ============================================================
cd /d %~dp0..

if not exist res\procpack.bin (
    echo ! res\procpack.bin отсутствует - сгенерируй через pack_procs.ps1
    exit /b 1
)

windres res\portable.rc -O coff -o res\portable.o || exit /b 1

g++ -O2 -mwindows -std=c++17 -DEMU_LIB=1 ^
    -finput-charset=UTF-8 -fexec-charset=WINDOWS-1251 ^
    -static -static-libgcc -static-libstdc++ ^
    -o gui\avrstudio.exe gui\main.cpp build\emu.cpp res\portable.o ^
    -lcomctl32 -lcomdlg32 -lgdi32 -lshell32 -luser32 -lole32 || exit /b 1

echo ok: gui\avrstudio.exe

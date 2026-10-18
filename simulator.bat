@echo off
rem AVR CLI Studio - pryamoy zapusk konsoli emulyatora (dash, iotree, trc, uart...)
cd /d "%~dp0"
title AVR Simulator
build\emu.exe --root .
pause

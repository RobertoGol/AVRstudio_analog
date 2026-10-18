@echo off
rem AVR CLI Studio - glavnaya obolochka (menyu proyekta, sborka, emulyator)
cd /d "%~dp0"
title AVR CLI Studio 4 - project shell
powershell -NoProfile -ExecutionPolicy Bypass -File avr.ps1
pause

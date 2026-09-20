@echo off
title AetherOS // Neural CLI
cd /d "%~dp0"
set "PATH=%~dp0;%~dp0src\llama-runtime\lib;%PATH%"

if not exist "aos.exe" (
    echo [ERROR] aos.exe not found!
    echo Please run build_windows.bat first.
    pause
    exit /b 1
)

aos.exe %*

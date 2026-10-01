@echo off
title AetherOS // Native Interface
cd /d "%~dp0"

:: Add DLL directories to Windows PATH
set "PATH=%~dp0src\llama-runtime\lib;%~dp0src\llama-runtime\bin;%PATH%"

if not exist "aos_imgui.exe" (
    echo [ERROR] aos_imgui.exe not found!
    echo Please compile it first using build_windows.bat
    echo.
    pause
    exit /b 1
)

echo Starting AetherOS Native Interface...
start "" aos_imgui.exe

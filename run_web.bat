@echo off
title AetherOS // Neural Web Server
cd /d "%~dp0"

:: Add DLL directories to Windows PATH
set "PATH=%~dp0src\llama-runtime\lib;%~dp0src\llama-runtime\bin;%PATH%"

if not exist "web_server.exe" (
    echo [ERROR] web_server.exe not found!
    echo Please compile it first using build_windows.bat
    echo.
    pause
    exit /b 1
)

echo ========================================================
echo   AETHER-OS LOCAL NEURAL SERVER (WINDOWS)
echo   Local URL: http://localhost:8080
echo ========================================================
echo.
echo Opening default browser...

:: Open browser automatically
start http://localhost:8080

:: Run the server
web_server.exe

pause

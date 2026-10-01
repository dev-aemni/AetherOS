@echo off
title AetherOS // Windows Build & Global Setup (@dev-aemni)
cd /d "%~dp0"

echo ========================================================
echo   AetherOS Windows Build & Global Setup (@dev-aemni)
echo   Repo: https://github.com/dev-aemni/AetherOS
echo ========================================================
echo.

:: 1. Check for portable compiler in pen drive, then system g++
if exist "w64devkit\bin\g++.exe" set "PATH=%~dp0w64devkit\bin;%PATH%"

where g++ >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] No Windows C++ compiler found!
    echo Please make sure w64devkit folder is present or MinGW is installed.
    pause
    exit /b 1
)

echo [1/3] Compiling aos.exe (CLI Mode)...
g++ -std=c++17 -O3 src\cli.cpp ^
    -Isrc\llama-runtime\include ^
    -Lsrc\llama-runtime\lib ^
    -lllama -lggml -lggml-base -lggml-cpu ^
    -o aos.exe

echo [2/3] Compiling web_server.exe (Mobile/Desktop Web Engine)...
g++ -std=c++17 -O3 web\server.cpp ^
    -Isrc\llama-runtime\include -Iweb ^
    -Lsrc\llama-runtime\lib ^
    -lllama -lggml -lggml-base -lggml-cpu -lws2_32 -lcrypt32 ^
    -o web_server.exe

echo [3/3] Compiling aos_imgui.exe (Native Cyberpunk GUI)...
g++ -std=c++17 -O3 src\aos_imgui.cpp ^
    src\imgui\imgui.cpp src\imgui\imgui_draw.cpp ^
    src\imgui\imgui_tables.cpp src\imgui\imgui_widgets.cpp ^
    src\imgui\backends\imgui_impl_sdl3.cpp ^
    src\imgui\backends\imgui_impl_sdlrenderer3.cpp ^
    -Isrc\llama-runtime\include -Isrc\imgui -Isrc\imgui\backends ^
    -Lsrc\llama-runtime\lib ^
    -lllama -lggml -lggml-base -lggml-cpu -lSDL3 ^
    -o aos_imgui.exe

echo.
echo ========================================================
echo   Configuring Global Windows Environment
echo ========================================================

:: 2. Safely add this directory to the User PATH persistently
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$dir = '%~dp0'.TrimEnd('\'); " ^
    "$userPath = [Environment]::GetEnvironmentVariable('Path', 'User'); " ^
    "if ($userPath -notlike ('*' + $dir + '*')) { " ^
    "    $newPath = ($userPath.TrimEnd(';') + ';' + $dir).TrimStart(';'); " ^
    "    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User'); " ^
    "    Write-Host '[SUCCESS] AetherOS added to User PATH permanently!' -ForegroundColor Green; " ^
    "} else { " ^
    "    Write-Host '[INFO] AetherOS is already registered in PATH.' -ForegroundColor Yellow; " ^
    "}"

echo.
echo ========================================================
echo [DONE] You can now open ANY new CMD/PowerShell and type:
echo.
echo    aos "your query"
echo    aos
echo ========================================================
pause

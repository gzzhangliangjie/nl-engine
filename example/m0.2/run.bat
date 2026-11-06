@echo off
setlocal
rem ==== M0.2 / s0-device successful cases ====================================
rem Double backend clear + resize regression. PNGs go to build\showcase\runs
rem so the baseline images in example\m0.2 stay untouched.
rem usage: run.bat [all^|clear^|resize^|color]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\showcase\s0-device\build\DX12\viewer.exe"
set "VK=%ROOT%\build\showcase\s0-device\build\VULKAN\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="clear" goto clear
if /i "%CASE%"=="resize" goto resize
if /i "%CASE%"=="color" goto color
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|clear^|resize^|color]
exit /b 1

:all
call :clear || exit /b 1
call :resize || exit /b 1
call :color || exit /b 1
exit /b 0

:clear
echo === M0.2 clear: DX12 + Vulkan, MD5 must match ===
"%DX%" --screenshot "%OUT%\m0.2-clear-DX12.png" --frames 8 || exit /b 1
"%VK%" --screenshot "%OUT%\m0.2-clear-VULKAN.png" --frames 8 || exit /b 1
echo output: %OUT%\m0.2-clear-*.png
exit /b 0

:resize
echo === M0.2 resize 640x360 -^> 1024x768 ===
"%DX%" --screenshot "%OUT%\m0.2-resize-DX12.png" --frames 8 --resize 1024x768 || exit /b 1
"%VK%" --screenshot "%OUT%\m0.2-resize-VULKAN.png" --frames 8 --resize 1024x768 || exit /b 1
exit /b 0

:color
echo === M0.2 clear color switch (--color 1) ===
"%DX%" --screenshot "%OUT%\m0.2-color-DX12.png" --frames 4 --color 1 || exit /b 1
"%VK%" --screenshot "%OUT%\m0.2-color-VULKAN.png" --frames 4 --color 1 || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Rebuild historical snapshots: python tools\showcase\rebuild.py
exit /b 1

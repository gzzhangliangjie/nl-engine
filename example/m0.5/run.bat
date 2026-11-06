@echo off
setlocal
rem ==== M0.5 / s0-done successful cases =====================================
rem ImGui HUD on both backends, HUD control states, no-HUD parity, HUD + resize, selftest.
rem usage: run.bat [all^|hud^|controls^|nohud^|resize^|selftest]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\dx12\viewer.exe"
set "VK=%ROOT%\build\vulkan\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="hud" goto hud
if /i "%CASE%"=="controls" goto controls
if /i "%CASE%"=="nohud" goto nohud
if /i "%CASE%"=="resize" goto resize
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|hud^|controls^|nohud^|resize^|selftest]
exit /b 1

:all
call :hud || exit /b 1
call :controls || exit /b 1
call :nohud || exit /b 1
call :resize || exit /b 1
call :selftest || exit /b 1
exit /b 0

:hud
echo === M0.5 ImGui HUD (960x540, deterministic timing) ===
"%DX%" --screenshot "%OUT%\m0.5-hud-DX12.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
"%VK%" --screenshot "%OUT%\m0.5-hud-VULKAN.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
echo compare: the two PNGs differ intentionally - the HUD prints the backend + debug layer names
exit /b 0

:controls
echo === M0.5 HUD control state: clear color=2 (green) + cull checked, Suzanne still visible ===
"%DX%" --screenshot "%OUT%\m0.5-controls-DX12.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --color 2 || exit /b 1
"%VK%" --screenshot "%OUT%\m0.5-controls-VULKAN.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --color 2 || exit /b 1
echo compare with the 'hud' case (color=0, deep blue): HudFrame drives both the widget and the clear color
exit /b 0

:nohud
echo === M0.5 no-HUD parity: pixel-identical to the M0.4 baseline on both backends ===
"%DX%" --screenshot "%OUT%\m0.5-nohud-DX12.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
"%VK%" --screenshot "%OUT%\m0.5-nohud-VULKAN.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
echo expected MD5 both: 7e528087d19722d8f9ef4de35e36d382
exit /b 0

:resize
echo === M0.5 HUD + resize 640x360 -^> 1024x768 (HUD pipeline rebuilt too) ===
"%DX%" --screenshot "%OUT%\m0.5-resize-DX12.png" --frames 8 --hud --size 640x360 --resize 1024x768 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
"%VK%" --screenshot "%OUT%\m0.5-resize-VULKAN.png" --frames 8 --hud --size 640x360 --resize 1024x768 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
exit /b 0

:selftest
echo === M0.5 selftest (14/14 each backend, incl. 8 shader artifacts) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

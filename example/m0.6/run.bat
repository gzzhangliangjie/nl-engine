@echo off
setlocal
rem ==== M0.6 successful cases ===============================================
rem Real GPU frame timing (NVRHI timestamp query) on both backends + no-HUD parity + selftest.
rem usage: run.bat [all^|hud^|nohud^|selftest]
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
if /i "%CASE%"=="nohud" goto nohud
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|hud^|nohud^|selftest]
exit /b 1

:all
call :hud || exit /b 1
call :nohud || exit /b 1
call :selftest || exit /b 1
exit /b 0

:hud
echo === M0.6 GPU frame timing HUD (960x540, fixed GPU/CPU samples for replay) ===
"%DX%" --screenshot "%OUT%\m0.6-hud-DX12.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
"%VK%" --screenshot "%OUT%\m0.6-hud-VULKAN.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
echo panel shows: GPU ms line + gpu ms curve (NVRHI timer) beside the CPU frame ms curve
echo compare: the two PNGs differ intentionally - the HUD prints the backend + debug layer names
exit /b 0

:nohud
echo === M0.6 no-HUD parity: timer queries never created, M0.4/M0.5 baseline byte-identical ===
"%DX%" --screenshot "%OUT%\m0.6-nohud-DX12.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
"%VK%" --screenshot "%OUT%\m0.6-nohud-VULKAN.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
echo expected MD5 both: 7e528087d19722d8f9ef4de35e36d382
exit /b 0

:selftest
echo === M0.6 selftest (14/14 each backend; HUD selftest covers s-to-ms conversion + ring cursor) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

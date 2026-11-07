@echo off
setlocal
rem ==== M0.9 successful cases ===============================================
rem M0.9 = one-command full regression gate (tools/regression.py): re-runs every
rem auto case in example/cases.json x both backends from tag snapshots, compares
rem PNGs against baselines, checks selftest logs + deviceErrors=0, exit 0 = green.
rem usage: run.bat [all^|gate-hud^|selftest^|regression]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\dx12\viewer.exe"
set "VK=%ROOT%\build\vulkan\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="gate-hud" goto gatehud
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="regression" goto regression
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|gate-hud^|selftest^|regression]
exit /b 1

:all
call :gatehud || exit /b 1
call :selftest || exit /b 1
exit /b 0

:gatehud
echo === M0.9 combo smoke: wireframe x tri-id x HUD in one frame ===
"%DX%" --screenshot "%OUT%\m0.9-gatehud-DX12.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --wireframe --shading tri-id || exit /b 1
"%VK%" --screenshot "%OUT%\m0.9-gatehud-VULKAN.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --wireframe --shading tri-id || exit /b 1
echo expected: colored wireframe Suzanne (per-triangle hash colors); HUD shows tri-id + wireframe ticked
exit /b 0

:selftest
echo === M0.9 selftest (15/15 each backend; stage check now expects M0.9) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:regression
echo === M0.9 full regression gate: every case x both backends ===
python "%ROOT%\tools\regression.py" || exit /b 1
echo expected: ALL GREEN, report at build\regression\report.md
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

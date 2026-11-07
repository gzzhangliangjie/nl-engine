@echo off
setlocal
rem ==== S1.0 successful cases ===============================================
rem Cluster visualization: --mode cluster loads assets/meshlets/*.nmesh and
rem colors every meshlet with a CPU-baked pcg hash color (identical bytes on
rem both backends). N key cycles shading (all 5 modes work in cluster scene).
rem usage: run.bat [all^|clusters^|hud^|selftest]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\dx12\viewer.exe"
set "VK=%ROOT%\build\vulkan\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="clusters" goto clusters
if /i "%CASE%"=="hud" goto hud
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|clusters^|hud^|selftest]
exit /b 1

:all
call :clusters || exit /b 1
call :hud || exit /b 1
call :selftest || exit /b 1
exit /b 0

:clusters
echo === S1.0 IcoSphereDense.nmesh cluster colors, no HUD ===
"%DX%" --mode cluster --screenshot "%OUT%\s1.0-clusters-DX12.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --model IcoSphereDense.nmesh || exit /b 1
"%VK%" --mode cluster --screenshot "%OUT%\s1.0-clusters-VULKAN.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --model IcoSphereDense.nmesh || exit /b 1
echo expected: dense sphere covered in uniform random cluster color blocks; DX12/Vulkan PNG byte-identical
exit /b 0

:hud
echo === S1.0 Suzanne.nmesh cluster colors + HUD ===
"%DX%" --mode cluster --screenshot "%OUT%\s1.0-hud-DX12.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
"%VK%" --mode cluster --screenshot "%OUT%\s1.0-hud-VULKAN.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
echo expected: Suzanne in compact cluster color blocks (no long malformed strips); HUD shows mode cluster / model Suzanne.nmesh
exit /b 0

:selftest
echo === S1.0 selftest (15/15 each backend; stage check now expects S1.0) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

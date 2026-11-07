@echo off
setlocal
rem ==== M0.7 successful cases ===============================================
rem Dense procedural test assets via --model (TorusKnotDense 204800 tri /
rem IcoSphereDense 327680 tri) + selftest. Generator: scripts\gen-dense-mesh.py
rem usage: run.bat [all^|hud^|icosphere^|selftest]
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
if /i "%CASE%"=="icosphere" goto icosphere
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|hud^|icosphere^|selftest]
exit /b 1

:all
call :hud || exit /b 1
call :icosphere || exit /b 1
call :selftest || exit /b 1
exit /b 0

:hud
echo === M0.7 TorusKnotDense via --model, HUD shows model name + 204800 tri ===
"%DX%" --screenshot "%OUT%\m0.7-hud-DX12.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 5.5 --cull back --model TorusKnotDense.glb || exit /b 1
"%VK%" --screenshot "%OUT%\m0.7-hud-VULKAN.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 5.5 --cull back --model TorusKnotDense.glb || exit /b 1
echo panel shows: model : TorusKnotDense.glb (204800 tri)
echo regenerate asset (deterministic): python scripts\gen-dense-mesh.py --shape torusknot --out assets\models\TorusKnotDense.glb
exit /b 0

:icosphere
echo === M0.7 IcoSphereDense via --model, no HUD ===
"%DX%" --screenshot "%OUT%\m0.7-icosphere-DX12.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back --model IcoSphereDense.glb || exit /b 1
"%VK%" --screenshot "%OUT%\m0.7-icosphere-VULKAN.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back --model IcoSphereDense.glb || exit /b 1
echo expected: 327680-tri subdivided icosphere with checker UV (seam line is expected)
exit /b 0

:selftest
echo === M0.7 selftest (14/14 each backend; stage check now expects M0.7) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

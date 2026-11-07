@echo off
setlocal
rem ==== M0.8 successful cases ===============================================
rem Debug shading (--shading textured|normal|uv|tri-id) + wireframe (--wireframe
rem / V key / HUD checkbox) / N key cycles shading. Wireframe = raster fill mode.
rem usage: run.bat [all^|hud^|wire^|uv^|tris^|selftest]
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
if /i "%CASE%"=="wire" goto wire
if /i "%CASE%"=="uv" goto uv
if /i "%CASE%"=="tris" goto tris
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|hud^|wire^|uv^|tris^|selftest]
exit /b 1

:all
call :hud || exit /b 1
call :wire || exit /b 1
call :uv || exit /b 1
call :tris || exit /b 1
call :selftest || exit /b 1
exit /b 0

:hud
echo === M0.8 TorusKnotDense normal shading + HUD shows shading combo ===
"%DX%" --screenshot "%OUT%\m0.8-hud-DX12.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 5.5 --cull back --model TorusKnotDense.glb --shading normal || exit /b 1
"%VK%" --screenshot "%OUT%\m0.8-hud-VULKAN.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 5.5 --cull back --model TorusKnotDense.glb --shading normal || exit /b 1
echo expected: RGB normal gradient on the dense knot; HUD combo shows "normal"
exit /b 0

:wire
echo === M0.8 Suzanne wireframe + HUD shows wireframe checked ===
"%DX%" --screenshot "%OUT%\m0.8-wire-DX12.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --wireframe || exit /b 1
"%VK%" --screenshot "%OUT%\m0.8-wire-VULKAN.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --wireframe || exit /b 1
echo expected: Suzanne rendered as wireframe mesh (3936 tri countable); checkbox ticked
exit /b 0

:uv
echo === M0.8 IcoSphereDense uv shading, no HUD ===
"%DX%" --screenshot "%OUT%\m0.8-uv-DX12.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back --model IcoSphereDense.glb --shading uv || exit /b 1
"%VK%" --screenshot "%OUT%\m0.8-uv-VULKAN.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back --model IcoSphereDense.glb --shading uv || exit /b 1
echo expected: u->red / v->green gradient sphere; seam continuity checkable
exit /b 0

:tris
echo === M0.8 Suzanne triangle-id shading, no HUD ===
"%DX%" --screenshot "%OUT%\m0.8-tris-DX12.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back --shading tri-id || exit /b 1
"%VK%" --screenshot "%OUT%\m0.8-tris-VULKAN.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back --shading tri-id || exit /b 1
echo expected: every triangle a distinct deterministic hash color (SV_PrimitiveID)
exit /b 0

:selftest
echo === M0.8 selftest (15/15 each backend; stage check now expects M0.8) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

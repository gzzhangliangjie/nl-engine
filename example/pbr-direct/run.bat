@echo off
setlocal
rem ==== pbr-direct successful cases =========================================
rem Cook-Torrance PBR direct lighting (mode 5, psMain only) + glTF
rem metallicRoughness textures (t0 baseColor UNORM + t1 MR linear + b1
rem LightConstants). --pbr-naive = acceptance A/B: skips sRGB->linear (naive
rem UNORM read, visibly washed-out bright).
rem usage: run.bat [all^|pbr^|naive^|hud^|textured^|selftest]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\DX12\viewer.exe"
set "VK=%ROOT%\build\VULKAN\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="pbr" goto pbr
if /i "%CASE%"=="naive" goto naive
if /i "%CASE%"=="hud" goto hud
if /i "%CASE%"=="textured" goto textured
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|pbr^|naive^|hud^|textured^|selftest]
exit /b 1

:all
call :pbr || exit /b 1
call :naive || exit /b 1
call :hud || exit /b 1
call :textured || exit /b 1
call :selftest || exit /b 1
exit /b 0

:pbr
echo === pbr-direct Suzanne Cook-Torrance PBR (correct sRGB path) ===
"%DX%" --screenshot "%OUT%\pbr-direct-pbr-DX12.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --shading pbr || exit /b 1
"%VK%" --screenshot "%OUT%\pbr-direct-pbr-VULKAN.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --shading pbr || exit /b 1
echo expected: dark metal body (glTF metallic=0.98, no IBL by design), gold
echo           eyes with GGX highlight sheen; two backends byte-identical
exit /b 0

:naive
echo === pbr-direct A/B: --pbr-naive skips sRGB-to-linear (washed-out bright) ===
"%DX%" --screenshot "%OUT%\pbr-direct-naive-DX12.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --shading pbr --pbr-naive || exit /b 1
"%VK%" --screenshot "%OUT%\pbr-direct-naive-VULKAN.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --shading pbr --pbr-naive || exit /b 1
echo expected: visibly brighter washed-out gray vs the correct :pbr shot
echo           (sRGB values read as linear = over-bright, the classic error)
exit /b 0

:hud
echo === pbr-direct HUD: shading combo shows "pbr", panel fits 960x540 ===
"%DX%" --screenshot "%OUT%\pbr-direct-hud-DX12.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --shading pbr || exit /b 1
"%VK%" --screenshot "%OUT%\pbr-direct-hud-VULKAN.png" --frames 8 --hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 --cull back --shading pbr || exit /b 1
echo expected: HUD combo "pbr", stage line "pbr-direct ...", panel bottom
echo           (reset camera + help) fully visible - not clipped
exit /b 0

:textured
echo === invariant check: --shading textured baseline must stay byte-identical ===
"%DX%" --screenshot "%OUT%\pbr-direct-textured-DX12.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
"%VK%" --screenshot "%OUT%\pbr-direct-textured-VULKAN.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
echo expected: MD5 7e528087d19722d8f9ef4de35e36d382 on BOTH backends
echo           (= example/m0.4/suzanne-*.png, M0.8-era behavior unchanged)
exit /b 0

:selftest
echo === pbr-direct selftest (16/16 each backend; stage check expects pbr-direct) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

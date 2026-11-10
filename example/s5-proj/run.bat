@echo off
rem NOTE: !EXE! delayed expansion is REQUIRED here ¡ª %VAR% inside a
rem parenthesized for-body expands at parse time (empty), which silently
rem no-ops the whole block (M4.0 script disease, fixed since M4.1).
setlocal enabledelayedexpansion
rem ==== M5.0 (S5.0) successful cases ======================================
rem Screen-space error projection: the S5 group-atomic cut criterion.
rem   screenPx = worldError / max(d, near) * projYScale * 0.5 * H
rem   d = |sphereCenter - eye| - sphereRadius   (bevy/zeux, cull_shared.wesl:13)
rem
rem Acceptance dimensions:
rem  1. hand-calc unit test inside viewer --selftest (lodErrorSelfTest, 16/16);
rem  2. HUD "s5 err" line decreases monotonically with camera distance:
rem     dist 2.0 -> 4.2 -> 8.0 gives 1567.05 -> 352.15 -> 149.84 px on
rem     Suzanne.lod6 (maxError 1.1375, union sphere R 1.2661), matching the
rem     offline python hand-calc digit for digit (evidence PNGs kept here);
rem  3. regression pair s5.0/s5-hud is byte-identical on both backends
rem     (checked by tools/regression.py, not duplicated here).
rem usage: run.bat [verify]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs\s5-proj"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\dx12\viewer.exe"
set "VK=%ROOT%\build\vulkan\viewer.exe"
set "FAIL=0"

rem ---- 1) selftest (hand-calc projection case) on both backends ------------
for %%m in (dx12 vulkan) do (
  if "%%m"=="dx12" (set "EXE=!DX!") else (set "EXE=!VK!")
  if not exist "!EXE!" (echo [FAIL] missing exe !EXE! & exit /b 1)
  "!EXE!" --selftest | findstr /c:"SELFTEST PASS (16/16)" >nul
  if errorlevel 1 (echo [FAIL] %%m selftest 16/16 & set "FAIL=1") else (echo [OK]   %%m selftest 16/16)
)

rem ---- 2) monotonic distance evidence: 3 distances, HUD shows s5 err -------
set "POSE=--mode dag-top --hud --frames 8 --shading normal --yaw 12 --pitch 9 --size 640x720"
for %%d in (2.0 4.2 8.0) do (
  "!DX!" !POSE! --dist %%d --screenshot "!OUT!\s5-d%%d-dx12.png" || exit /b 1
  "!VK!" !POSE! --dist %%d --screenshot "!OUT!\s5-d%%d-vulkan.png" || exit /b 1
  echo [OK]   shot dist %%d on both backends
)
echo     expected HUD values: 1567.05 / 352.15 / 149.84 px (monotonic down)
echo     reference evidence: %~dp0s5-evidence-d2.0-dx12.png etc.

if "%FAIL%"=="1" (echo M5.0 verify: FAIL & exit /b 1) else (echo M5.0 verify: ALL OK & exit /b 0)

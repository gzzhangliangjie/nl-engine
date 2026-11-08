@echo off
rem NOTE: !EXE!/!FRONT!/!INSIDE! delayed expansion is REQUIRED here — %VAR%
rem inside a parenthesized for-body expands at parse time (empty), which made
rem the s4.0 script execute a literal "" command and silently no-op (M4.1 fix).
setlocal enabledelayedexpansion
rem ==== M4.2 (S4.2) successful cases ======================================
rem 1.3M-tri dense scene (IcoSphereDense1M.nmesh: 19718 clusters / 1310720
rem tri) + cull pass GPU timing (second GpuTimer slot, HUD "cull GPU" row).
rem
rem Acceptance dimensions:
rem  1. pass-through on 1.3M tri: front pose (640x360 -> 19718/19718 ALL
rem     visible) renders byte-for-byte identical to the cluster path;
rem     at 640x720 the frustum additionally culles 1353 clusters with ZERO
rem     pixel impact (conservative sphere test, see MILESTONES M4.2);
rem  2. real culling: camera inside the sphere (dist 0.45, 640x720) ->
rem     GPU count 1932/19718 (90.2% culled) matches frustum_check.py
rem     offline reference and the HUD "frustum" stats line;
rem  3. inner wall renders (cull none) - the 1932-cluster visible list
rem     actually rasterizes (full-screen cluster mosaic);
rem  4. offline tool == GPU on both backends;
rem  5. perf (60fps, measured live, see MILESTONES M4.2): CPU avg 4.9-5.5ms,
rem     whole-frame GPU 0.96-1.02ms front / 0.17ms inside, cull pass
rem     ~0.010ms - two orders of magnitude under the 16.6ms budget.
rem usage: run.bat [verify]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\dx12\viewer.exe"
set "VK=%ROOT%\build\vulkan\viewer.exe"
call :need "%DX%" || exit /b 1
call :need "%VK%" || exit /b 1
set "MODEL=IcoSphereDense1M.nmesh"
set "FRONT=--no-hud --frames 4 --shading cluster --yaw 30 --pitch 20 --dist 2.6 --size 640x360"
set "INSIDE=--hud --frames 6 --shading cluster --yaw 30 --pitch 20 --dist 0.45 --size 640x720"
set "WALL=--no-hud --frames 4 --shading cluster --yaw 30 --pitch 20 --dist 0.45 --cull none --size 640x720"
set "FAIL=0"

rem ---- 1) pass-through on 1.3M tri: cull (frustum on) == cluster ----------
for %%m in (dx12 vulkan) do (
  if "%%m"=="dx12" (set "EXE=!DX!") else (set "EXE=!VK!")
  "!EXE!" --mode cluster --model !MODEL! !FRONT! --screenshot "%OUT%\s42-cluster-%%m.png" || exit /b 1
  "!EXE!" --mode cull    --model !MODEL! !FRONT! --screenshot "%OUT%\s42-cull-%%m.png"    || exit /b 1
  fc /b "%OUT%\s42-cluster-%%m.png" "%OUT%\s42-cull-%%m.png" >nul
  if errorlevel 1 (echo [FAIL] %%m front : cluster not equal cull & set "FAIL=1") else (echo [OK]   %%m front : cluster == cull on 1.3M tri)
)

rem ---- 2) real culling: HUD stats line shows the GPU count -----------------
for %%m in (dx12 vulkan) do (
  if "%%m"=="dx12" (set "EXE=!DX!") else (set "EXE=!VK!")
  "!EXE!" --mode cull --model !MODEL! !INSIDE! --screenshot "%OUT%\s42-inside-%%m.png" 2>&1 | findstr /c:"cull stats: visible 1932 / 19718" >nul
  if errorlevel 1 (echo [FAIL] %%m inside : cull stats not 1932/19718 & set "FAIL=1") else (echo [OK]   %%m inside : cull stats == 1932/19718 (90.2%% culled))
)

rem ---- 3) inner wall renders with cull none (visible list rasterizes) -----
for %%m in (dx12 vulkan) do (
  if "%%m"=="dx12" (set "EXE=!DX!") else (set "EXE=!VK!")
  "!EXE!" --mode cull --model !MODEL! !WALL! --screenshot "%OUT%\s42-wall-%%m.png" || exit /b 1
  if not exist "%OUT%\s42-wall-%%m.png" (echo [FAIL] %%m wall render missing & set "FAIL=1") else (echo [OK]   %%m wall : inner wall rendered)
)

rem ---- 4) offline reference == GPU -----------------------------------------
python "%ROOT%\tools\frustum_check.py" "%ROOT%\assets\meshlets\IcoSphereDense1M.nmesh" --yaw 30 --pitch 20 --dist 0.45 --width 640 --height 720 --expect 1932 >nul
if errorlevel 1 (echo [FAIL] offline inside != 1932 & set "FAIL=1") else (echo [OK]   offline inside == 1932)
python "%ROOT%\tools\frustum_check.py" "%ROOT%\assets\meshlets\IcoSphereDense1M.nmesh" --yaw 30 --pitch 20 --dist 2.6 --width 640 --height 360 --expect 19718 >nul
if errorlevel 1 (echo [FAIL] offline front != 19718 & set "FAIL=1") else (echo [OK]   offline front == 19718 - all visible at 640x360)

if "%FAIL%"=="1" (echo M4.2 acceptance FAILED & exit /b 1)
echo M4.2 acceptance PASSED: 1.3M-tri pass-through == cluster + inside 1932/19718 GPU==offline + wall renders (DX12+Vulkan)
exit /b 0

:need
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

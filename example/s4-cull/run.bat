@echo off
rem NOTE: !EXE!/!FRONT!/!INSIDE! delayed expansion is REQUIRED here ¡ª %VAR%
rem inside a parenthesized for-body expands at parse time (empty), which used
rem to make the s4.0 script execute a literal "" command and silently no-op.
setlocal enabledelayedexpansion
rem ==== M4.1 (S4.1) successful cases ======================================
rem viewer --mode cull now runs the GPU compute frustum cull pass: one thread
rem per cluster sphere (5-plane Gribb-Hartmann test), atomicAdd append into a
rem visible list, drawIndexedIndirect consumes it (visible-list indirection).
rem
rem Acceptance dimensions:
rem  1. pass-through: front pose (all 237 clusters visible) renders byte-for-
rem     byte identical to the traditional cluster path -- the cull pass +
rem     indirection has zero pixel impact when nothing is culled;
rem  2. real culling: camera inside the model (dist 0.45, 640x720) -> GPU
rem     count 30/237 matches tools/frustum_check.py offline reference and the
rem     HUD "frustum" stats line (2-frame delayed readback, frames>=4);
rem  3. offline tool == GPU on both backends.
rem usage: run.bat [verify]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\dx12\viewer.exe"
set "VK=%ROOT%\build\vulkan\viewer.exe"
call :need "%DX%" || exit /b 1
call :need "%VK%" || exit /b 1
set "FRONT=--no-hud --frames 4 --shading cluster --yaw 30 --pitch 20 --dist 2.6 --size 640x360"
set "INSIDE=--hud --frames 4 --yaw 12 --pitch 9.2 --dist 0.45 --size 640x720"
set "FAIL=0"

rem ---- 1) pass-through: cull (frustum on) == cluster, both backends --------
for %%m in (dx12 vulkan) do (
  if "%%m"=="dx12" (set "EXE=%DX%") else (set "EXE=%VK%")
  "!EXE!" --mode cluster !FRONT! --screenshot "%OUT%\s41-cluster-%%m.png" || exit /b 1
  "!EXE!" --mode cull    !FRONT! --screenshot "%OUT%\s41-cull-%%m.png"    || exit /b 1
  fc /b "%OUT%\s41-cluster-%%m.png" "%OUT%\s41-cull-%%m.png" >nul
  if errorlevel 1 (echo [FAIL] %%m front : cluster not equal cull & set "FAIL=1") else (echo [OK]   %%m front : cluster == cull)
)

rem ---- 2) real culling: HUD stats line shows the GPU count -----------------
for %%m in (dx12 vulkan) do (
  if "%%m"=="dx12" (set "EXE=%DX%") else (set "EXE=%VK%")
  "!EXE!" --mode cull !INSIDE! --screenshot "%OUT%\s41-inside-%%m.png" 2>&1 | findstr /c:"cull stats: visible 30 / 237" >nul
  if errorlevel 1 (echo [FAIL] %%m inside : cull stats not 30/237 & set "FAIL=1") else (echo [OK]   %%m inside : cull stats == 30/237)
)

rem ---- 3) offline reference == GPU -----------------------------------------
python "%ROOT%\tools\frustum_check.py" "%ROOT%\assets\meshlets\Suzanne.nmesh" --yaw 12 --pitch 9.2 --dist 0.45 --width 640 --height 720 --expect 30 >nul
if errorlevel 1 (echo [FAIL] offline inside != 30 & set "FAIL=1") else (echo [OK]   offline inside == 30)
python "%ROOT%\tools\frustum_check.py" "%ROOT%\assets\meshlets\Suzanne.nmesh" --yaw 12 --pitch 9.2 --dist 4.2 --expect 237 >nul
if errorlevel 1 (echo [FAIL] offline front != 237 & set "FAIL=1") else (echo [OK]   offline front == 237)

if "%FAIL%"=="1" (echo M4.1 acceptance FAILED & exit /b 1)
echo M4.1 acceptance PASSED: pass-through == cluster + inside 30/237 GPU==offline (DX12+Vulkan)
exit /b 0

:need
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

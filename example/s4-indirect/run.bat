@echo off
rem NOTE: !EXE!/!CAM! delayed expansion is REQUIRED — %VAR% inside a
rem parenthesized for-body expands at parse time (empty), which made this
rem script execute a literal "" command and silently no-op (fixed in M4.1).
setlocal enabledelayedexpansion
rem ==== M4.0 (S4.0) successful cases ======================================
rem viewer --mode cull: GPU vertex pulling (single drawIndexedIndirect,
rem instanceCount = total cluster count = full draw, no culling yet). The
rem acceptance is that this pulling path reproduces the traditional
rem vertex-buffer cluster path (--mode cluster) byte-for-byte for the same
rem camera + shading. M4.1 will replace the full-draw instanceCount with
rem GPU frustum-culling output.
rem usage: run.bat [verify]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\dx12\viewer.exe"
set "VK=%ROOT%\build\vulkan\viewer.exe"
call :need "%DX%" || exit /b 1
call :need "%VK%" || exit /b 1
set "CAM=--no-hud --frames 1 --shading cluster --yaw 30 --pitch 20 --dist 2.6 --size 640x360"
set "FAIL=0"
for %%m in (dx12 vulkan) do (
  if "%%m"=="dx12" (set "EXE=%DX%") else (set "EXE=%VK%")
  for %%c in (back off) do (
    "!EXE!" --mode cluster --cull %%c !CAM! --screenshot "%OUT%\s4-cluster-%%m-%%c.png" || exit /b 1
    "!EXE!" --mode cull    --cull %%c !CAM! --screenshot "%OUT%\s4-pulling-%%m-%%c.png" || exit /b 1
    fc /b "%OUT%\s4-cluster-%%m-%%c.png" "%OUT%\s4-pulling-%%m-%%c.png" >nul
    if errorlevel 1 (echo [FAIL] %%m %%c : cluster != pulling & set "FAIL=1") else (echo [OK]   %%m %%c : cluster == pulling)
  )
)
if "%FAIL%"=="1" (echo M4.0 acceptance FAILED & exit /b 1)
echo M4.0 acceptance PASSED: vertex pulling == cluster (DX12+Vulkan x back/none)
exit /b 0

:need
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

@echo off
setlocal
rem M8.0 (S8.0) BLAS prebuild from fallback mesh acceptance shots.
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).
rem Usage: run.bat verify   (re-shots blas-hud pair + BLAS log evidence, exit 0 = ok)
rem
rem Evidence chain:
rem   - hiz mode on Suzanne chain -> initDag picks levels[0] lod0 'NFLB' fallback
rem     (96 verts / 104 tris), builds one BLAS per mesh (flags=PreferFastTrace)
rem     in a dedicated command list (upload + setBufferState + build same list,
rem     0x87d lesson), GPU-timed. HUD shows "BLAS : 104 tri" (deterministic
rem     screenshots hide ms; real build ms goes to the log line below).
rem   - Machine-readable evidence: "blas built:" info line + one-shot
rem     "gpu timer resolved [blas]: X ms" (first successful timestamp query).
rem   - Vulkan RT extension chain: VK_KHR_acceleration_structure / ray_query /
rem     buffer_device_address wired through NVRHI deviceExtensions (M8.0).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] blas-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --hud --screenshot "%OUT%blas-hud-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 set FAIL=1
)
echo [check] dx12: blas built info line ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --hud --screenshot "%OUT%verify-log-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"blas built: Suzanne.lod0.nmesh | vertices=96 triangles=104"
if errorlevel 1 set FAIL=1
echo [check] dx12: one-shot blas GPU timer resolve ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --hud --screenshot "%OUT%verify-timer-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"gpu timer resolved [blas]"
if errorlevel 1 set FAIL=1
echo [check] vulkan: blas built info line ...
"%ROOT%\build\vulkan\viewer.exe" --mode hiz --hud --screenshot "%OUT%verify-log-vulkan.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"blas built: Suzanne.lod0.nmesh | vertices=96 triangles=104"
if errorlevel 1 set FAIL=1
echo [check] vulkan: one-shot blas GPU timer resolve ...
"%ROOT%\build\vulkan\viewer.exe" --mode hiz --hud --screenshot "%OUT%verify-timer-vulkan.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"gpu timer resolved [blas]"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s8-blas verify
  exit /b 1
)
echo [OK] s8-blas verify: blas-hud pair + build/timer log checks
exit /b 0

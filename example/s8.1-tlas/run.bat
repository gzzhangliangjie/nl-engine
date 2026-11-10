@echo off
setlocal
rem M8.1 (S8.1) TLAS per-frame rebuild + RayQuery probe acceptance shots.
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).
rem Usage: run.bat verify   (re-shots tlas-hud pair + rebuild/probe log evidence, exit 0 = ok)
rem
rem Evidence chain:
rem   - hiz mode on Suzanne chain -> initTlas after initBlas: top-level AS
rem     (maxInstances=16), rebuilt EVERY frame in the frame command list
rem     (D-S8-1: full rebuild each frame, UE convention). Per-frame cost is
rem     GPU-timed; HUD shows "TLAS : 1 inst" below the BLAS row.
rem   - Frame command list is persistent (M8.1 lesson): a per-frame created
rem     list destroyed its DXR scratch manager while the build ECL was still
rem     in flight -> debug layer CORRUPTION #921 (OutputDebugString only) ->
rem     scratch page reuse -> fail-fast 0x87D at frame 3. Vulkan keeps
rem     per-frame lists (NVRHI Vulkan manages pool lifetime queue-side).
rem   - Machine-readable evidence: "tlas: per-frame rebuild active" info line,
rem     one-shot "gpu timer resolved [tlas]: X ms", and with --tlas-spin
rem     + --tlas-probe: "tlas probe: seq=N t=X" lines (RayQuery fixed ray
rem     against the rebuilt TLAS; t changes as the instance rotates 2.5
rem     deg/frame -> per-frame rebuild actually takes effect).
rem
set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] tlas-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --hud --screenshot "%OUT%tlas-hud-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 set FAIL=1
  echo [shot] tlas-spin-probe on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --hud --tlas-spin --tlas-probe --screenshot "%OUT%tlas-spin-probe-%%B.png" --frames 24 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"tlas probe: seq"
  if errorlevel 1 set FAIL=1
)
echo [check] dx12: per-frame rebuild active line + one-shot tlas timer resolve ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --hud --screenshot "%OUT%verify-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"tlas: per-frame rebuild active | instances=1" /C:"gpu timer resolved [tlas]"
if errorlevel 1 set FAIL=1
echo [check] vulkan: per-frame rebuild active line + one-shot tlas timer resolve ...
"%ROOT%\build\vulkan\viewer.exe" --mode hiz --hud --screenshot "%OUT%verify-vulkan.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"tlas: per-frame rebuild active | instances=1" /C:"gpu timer resolved [tlas]"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s8.1-tlas verify
  exit /b 1
)
echo [OK] s8.1-tlas verify: tlas-hud pair + spin/probe + rebuild/timer log checks
exit /b 0

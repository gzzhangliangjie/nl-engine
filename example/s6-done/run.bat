@echo off
setlocal
rem M6.2 (S6.2) Hi-Z runtime toggle + pre/post cull counts acceptance shots.
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).
rem Usage: run.bat verify   (re-shoots toggle on/off pairs + occl1m pair, exit 0 = ok)
rem
rem Evidence chain:
rem   - HizScene front (yaw 12): --hiz on  -> visible 24/85, occluded 10 of 34
rem     visited; --hiz off -> visible 40/85 (== dag baseline), occluded 0 of 40.
rem     Toggle off falls back to the pure frustum/error traversal immediately.
rem   - OcclScene1M (1.3M-tri sphere + front wall, make-occl-scene-glb.py):
rem     --hiz on -> visible 6861/43547, occluded 5378 of 12239 visited;
rem     --hiz off -> visible 12324/43547, occluded 0. GPU frame time measured
rem     live at 1080p (unthrottled, NVRHI timer): on ~2.7ms vs off ~4.0ms (-32%).
rem   - hiz on/off final images are byte-identical (culled clusters were
rem     depth-hidden anyway) = no missed draws.

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] toggle front on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --model HizScene.nmesh --hud --hiz on --screenshot "%OUT%hiz-toggle-front-%%B.png" --frames 10 --yaw 12 --pitch 9 --dist 4.2 --size 960x540
  if errorlevel 1 set FAIL=1
  echo [shot] toggle front off %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --model HizScene.nmesh --hud --hiz off --screenshot "%OUT%hiz-toggle-off-%%B.png" --frames 10 --yaw 12 --pitch 9 --dist 4.2 --size 960x540
  if errorlevel 1 set FAIL=1
  echo [shot] toggle side on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --model HizScene.nmesh --hud --hiz on --screenshot "%OUT%hiz-toggle-side-%%B.png" --frames 10 --yaw 75 --pitch 5 --dist 4.2 --size 960x540
  if errorlevel 1 set FAIL=1
)
echo [shot] occl1m on/off dx12 ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model OcclScene1M.nmesh --hud --hiz on --screenshot "%OUT%occl1m-on-dx12.png" --frames 12 --yaw 0 --pitch 0 --dist 3.2 --size 960x540
if errorlevel 1 set FAIL=1
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model OcclScene1M.nmesh --hud --hiz off --screenshot "%OUT%occl1m-off-dx12.png" --frames 12 --yaw 0 --pitch 0 --dist 3.2 --size 960x540
if errorlevel 1 set FAIL=1

rem Machine-readable evidence: hiz stats log lines (readback ring, delayed 2
rem frames). Steady state from frame 3.
echo [check] hiz on: 24/85 with occluded 10 of 34 ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model HizScene.nmesh --hiz on --screenshot "%OUT%verify-on-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 960x540 | findstr /C:"visible 24 / 85 clusters, occluded 10 of 34 visited"
if errorlevel 1 set FAIL=1
echo [check] hiz off: falls back to dag baseline 40/85, occluded 0 ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model HizScene.nmesh --hiz off --screenshot "%OUT%verify-off-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 960x540 | findstr /C:"visible 40 / 85 clusters, occluded 0 of 40 visited"
if errorlevel 1 set FAIL=1
echo [check] hiz on side: zero occlusion at yaw 75 ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model HizScene.nmesh --hiz on --screenshot "%OUT%verify-side-dx12.png" --frames 8 --yaw 75 --pitch 5 --dist 4.2 --size 960x540 | findstr /C:"visible 40 / 85 clusters, occluded 0 of 40 visited"
if errorlevel 1 set FAIL=1
echo [check] occl1m on: 6861/43547 with occluded 5378 of 12239 ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model OcclScene1M.nmesh --hiz on --screenshot "%OUT%verify-occl-on-dx12.png" --frames 10 --yaw 0 --pitch 0 --dist 3.2 --size 960x540 | findstr /C:"visible 6861 / 43547 clusters, occluded 5378 of 12239 visited"
if errorlevel 1 set FAIL=1
echo [check] occl1m off: 12324/43547, occluded 0 ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model OcclScene1M.nmesh --hiz off --screenshot "%OUT%verify-occl-off-dx12.png" --frames 10 --yaw 0 --pitch 0 --dist 3.2 --size 960x540 | findstr /C:"visible 12324 / 43547 clusters, occluded 0 of 12324 visited"
if errorlevel 1 set FAIL=1
echo [check] occl1m on: GPU timing log present (perf evidence chain) ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model OcclScene1M.nmesh --hiz on --screenshot "%OUT%verify-occl-gpu-dx12.png" --frames 10 --yaw 0 --pitch 0 --dist 3.2 --size 960x540 | findstr /C:"gpu timing: avg"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s6-done verify
  exit /b 1
)
echo [OK] s6-done verify: toggle on/off pairs + occl1m pair + count/gpu checks
exit /b 0

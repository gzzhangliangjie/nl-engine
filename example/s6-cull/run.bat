@echo off
setlocal
rem M6.1 (S6.1) Hi-Z occlusion culling wired into DAG traversal acceptance shots.
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).
rem Usage: run.bat verify   (re-shoots hiz/dag front+side pairs, exit 0 = ok)
rem
rem Scene: assets/models/HizScene.glb = wall occluder + 3 spheres behind it +
rem 1 control sphere beside the wall edge (scripts/make-hiz-scene-glb.py).
rem Front view: wall hides the 3 spheres -> hiz culls them (24/85 vs dag 40/85).
rem Side view (yaw 75): wall no longer occludes -> hiz == dag == 40/85 (no missed
rem draws; hiz removed clusters are exactly the depth-hidden ones).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] hiz front %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --model HizScene.nmesh --hud --screenshot "%OUT%hiz-front-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 960x540
  if errorlevel 1 set FAIL=1
  echo [shot] hiz side %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --model HizScene.nmesh --hud --screenshot "%OUT%hiz-side-%%B.png" --frames 8 --yaw 75 --pitch 5 --dist 4.2 --size 960x540
  if errorlevel 1 set FAIL=1
)
echo [shot] dag front dx12 ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --model HizScene.nmesh --hud --screenshot "%OUT%dag-front-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 960x540
if errorlevel 1 set FAIL=1
echo [shot] dag side dx12 ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --model HizScene.nmesh --hud --screenshot "%OUT%dag-side-dx12.png" --frames 8 --yaw 75 --pitch 5 --dist 4.2 --size 960x540
if errorlevel 1 set FAIL=1

rem Machine-readable evidence: occlusion-culled cluster counts land in the log
rem lines (readback ring, delayed 2 frames). Steady state from frame 3.
echo [check] hiz front culls behind-wall clusters (expect 24 / 85) ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model HizScene.nmesh --hud --screenshot "%OUT%verify-front-hiz-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 960x540 | findstr /C:"visible 24 / 85"
if errorlevel 1 set FAIL=1
echo [check] dag front baseline without hiz (expect 40 / 85) ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --model HizScene.nmesh --hud --screenshot "%OUT%verify-front-dag-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 960x540 | findstr /C:"visible 40 / 85"
if errorlevel 1 set FAIL=1
echo [check] hiz side: nothing occluded, no missed draws (expect 40 / 85) ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --model HizScene.nmesh --hud --screenshot "%OUT%verify-side-hiz-dx12.png" --frames 8 --yaw 75 --pitch 5 --dist 4.2 --size 960x540 | findstr /C:"visible 40 / 85"
if errorlevel 1 set FAIL=1
echo [check] dag side baseline (expect 40 / 85) ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --model HizScene.nmesh --hud --screenshot "%OUT%verify-side-dag-dx12.png" --frames 8 --yaw 75 --pitch 5 --dist 4.2 --size 960x540 | findstr /C:"visible 40 / 85"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s6-cull verify
  exit /b 1
)
echo [OK] s6-cull verify: front pair + side pair + hiz 24/85 vs dag 40/85 count checks
exit /b 0

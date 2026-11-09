@echo off
setlocal
rem M6.0 (S6.0) Hi-Z depth prepass + hand-rolled min pyramid acceptance shots.
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).
rem Usage: run.bat verify   (re-shoots near/far viz pairs for both backends, exit 0 = ok)

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] near %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --hud --screenshot "%OUT%hiz-viz-near-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 set FAIL=1
  echo [shot] far %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --hud --screenshot "%OUT%hiz-viz-far-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 20 --size 640x720
  if errorlevel 1 set FAIL=1
)

rem Machine-readable evidence: pyramid top min lands in the log line (readback ring,
rem delayed 2 frames). near (dist 4.2) ~3.29 = nearest radial depth; far (dist 20)
rem ~19.07 grows with camera distance. Both backends bit-exact (M6.0 acceptance).
echo [check] near top min (expect 3.29...) ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --screenshot "%OUT%verify-near-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720 | findstr /C:"pyramid top min 3.29"
if errorlevel 1 set FAIL=1
echo [check] far top min (expect 19.06...) ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --screenshot "%OUT%verify-far-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 20 --size 640x720 | findstr /C:"pyramid top min 19.06"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s6-hiz verify
  exit /b 1
)
echo [OK] s6-hiz verify: near pair + far pair + top min checks
exit /b 0

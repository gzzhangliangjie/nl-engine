@echo off
setlocal
rem M-PT0 particle overlay acceptance shots + verify.
rem ASCII only. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12|VULKAN
rem
rem Evidence chain:
rem   - Fixed-step (1/60s) CPU fountain sim -> alive count is a pure function of
rem     frame count. 120 frames = 2s sim time -> alive=567 (rate 300/s within
rem     life window [1.6,2.3]s; theoretical spot value, dual-backend identical).
rem   - Determinism: two identical DX12 runs produce byte-identical PNGs.
rem   - HUD evidence row "Particles : 567 alive" only shows with --particles
rem     (negative card proves default-off keeps HUD layout untouched).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] particles-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --particles --hud --screenshot "%OUT%particles-hud-%%B.png" --frames 120 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 set FAIL=1
)

echo [check] dx12 determinism: second identical run must be byte-identical ...
"%ROOT%\build\dx12\viewer.exe" --particles --hud --screenshot "%OUT%verify-determinism.png" --frames 120 --yaw 12 --pitch 9 --dist 4.2 --size 640x720 > "%OUT%verify-run.log" 2>&1
fc /b "%OUT%particles-hud-dx12.png" "%OUT%verify-determinism.png" >nul
if errorlevel 1 (
  echo [ERROR] determinism broken: baseline vs rerun differ
  set FAIL=1
) else (
  echo determinism byte-identical OK
)

echo [check] steady log anchor, dx12 ...
"%ROOT%\build\dx12\viewer.exe" --particles --hud --screenshot "%OUT%verify-log-dx12.png" --frames 120 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"particles steady: alive=567"
if errorlevel 1 set FAIL=1

echo [check] steady log anchor, vulkan ...
"%ROOT%\build\vulkan\viewer.exe" --particles --hud --screenshot "%OUT%verify-log-vulkan.png" --frames 120 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"particles steady: alive=567"
if errorlevel 1 set FAIL=1

echo [check] negative: default-off run must NOT print the steady anchor ...
"%ROOT%\build\dx12\viewer.exe" --hud --screenshot "%OUT%verify-off-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"particles steady" >nul
if errorlevel 1 (
  echo negative OK: no steady line without --particles
) else (
  echo [ERROR] unexpected particles steady line in default-off run
  set FAIL=1
)

if %FAIL%==1 (
  echo [FAIL] particles-pt0 verify
  exit /b 1
)
echo [OK] particles-pt0 verify: hud pair + determinism + steady anchor + negative
exit /b 0

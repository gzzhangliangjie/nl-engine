@echo off
setlocal
rem M14A2/A3 (S14-A) SW/HW split baseline + blend-visible acceptance.
rem ASCII only; CRLF; no parentheses inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem Split mode is deterministic (same as soft-raster: u64 atomicMin, no RNG)
rem -> bytes gate vs frozen baseline (--sw-split, default threshold 64).
rem Default threshold 64 was confirmed by the M14A3 profile: split64 frame
rem cost 0.125 ms vs full-HW 0.117 ms (default camera) and 0.124 vs 0.162 ms
rem (dist18, -24 pct) while cutting far-field flicker 37 pct (A2 evidence).
rem Assertions:
rem   - re-shot --mode soft-raster --sw-split on both backends exits 0;
rem     dx12 deviceErrors=0
rem   - re-shot bytes-identical to frozen baseline (both backends)
rem   - blend-visible gate: --sw-threshold 8 re-shot (dx12) must differ from
rem     the th64 baseline (split actually re-routes clusters)
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --size 960x540 --yaw 12 --pitch 9 --dist 7 --frames 128 --no-hud --sw-split"
set FAIL=0

echo [shot] re-shoot split (default th64) on both backends (dx12 log captured)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode soft-raster %ARGS% --screenshot "%OUT%verify-split-%%B.png" > "%OUT%verify-log.txt" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode soft-raster %ARGS% --screenshot "%OUT%verify-split-%%B.png" >nul 2>&1
  )
  if errorlevel 1 (
    echo [FAIL] split-%%B viewer exited nonzero
    set FAIL=1
  )
)

findstr /C:"deviceErrors=0" "%OUT%verify-log.txt" <nul >nul
if errorlevel 1 (
    echo [FAIL] dx12 re-shot reported device errors
    set FAIL=1
) else (
    echo [OK] dx12 re-shot deviceErrors=0
)

echo [check] bytes: re-shot vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%split-%%B.png" "%OUT%verify-split-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] split-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] split-%%B bytes identical to baseline
  )
)

echo [check] blend-visible: th8 re-shot differs from th64 baseline
"%ROOT%\build\dx12\viewer.exe" --mode soft-raster --sw-split --sw-threshold 8 --model Suzanne-materials.nmesh --size 960x540 --yaw 12 --pitch 9 --dist 7 --frames 128 --no-hud --screenshot "%OUT%verify-split-th8.png" >nul 2>&1
fc /b "%OUT%split-dx12.png" "%OUT%verify-split-th8.png" >nul
if errorlevel 1 (
    echo [OK] th8 differs from th64 baseline: split re-routes clusters
) else (
    echo [FAIL] th8 identical to th64: blend gate dead
    set FAIL=1
)

echo [negative] missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode soft-raster --sw-split --model missing.nmesh --frames 1 --no-hud --screenshot "%OUT%verify-negative.png" >nul 2>&1
if errorlevel 1 (
    echo [OK] negative: missing model exits nonzero
) else (
    echo [FAIL] negative: missing model exited zero
    set FAIL=1
)

if "%FAIL%"=="1" (
    echo VERIFY FAILED
    exit /b 1
)
echo VERIFY OK
exit /b 0

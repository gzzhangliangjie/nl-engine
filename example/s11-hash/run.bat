@echo off
setlocal
rem S11 M11.0 - world cache (GPU flat hashmap) cache-viz baseline acceptance.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M11.0 delivers:
rem   --mode cache-viz : per-pixel query-only pseudo-color of the world cache
rem     hash grid (262144 slots, pcg slot key + iqint checksum, 0 = empty,
rem     linear probe 3 steps, life decay 10 / query refresh, bevy/Carrot
rem     reclaim semantics). Fill runs at half resolution with R2 jitter and
rem     inserts on query; viz rewrites every pixel (gamma domain, pow 1/2.2).
rem Assertions:
rem   - cache-viz re-shot on both backends exits 0, deviceErrors=0
rem   - re-shot == frozen baseline (bytes; deterministic fill+viz pipeline)
rem   - log anchor: cache viz compose dispatch
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--mode cache-viz --model Suzanne-materials.nmesh --frames 16 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7"
set FAIL=0

echo [shot] re-shoot cache-viz on both backends (dx12 log captured to verify-log.txt)
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --screenshot "%OUT%verify-cacheviz-%%B.png" > "%OUT%verify-log.txt" 2>&1
  if errorlevel 1 (
    echo [FAIL] cache-viz-%%B viewer exited nonzero
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

echo [check] log anchor: cache viz compose dispatch
findstr /C:"cache: viz compose dispatch" "%OUT%verify-log.txt" <nul >nul
if errorlevel 1 (
  echo [FAIL] viz compose anchor missing
  set FAIL=1
) else (
  echo [OK] viz compose anchor present
)

echo [check] bytes: re-shot vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%cacheviz-%%B.png" "%OUT%verify-cacheviz-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] cache-viz-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] cache-viz-%%B bytes identical to baseline
  )
)

echo [negative] missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode cache-viz --model missing.nmesh --frames 1 --no-hud --screenshot "%OUT%verify-negative.png" >nul 2>&1
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

@echo off
setlocal
rem M10.0 / S10 - screen probe cascade placement acceptance shots.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M10.0 --mode probe-place: independent velocity prepass (Q10-1 UE style) feeds
rem csPlace cascade placement - 4 levels (16/8/4/2, spacing doubles per level),
rem screen grid -> depth reconstruct world pos -> 3-tap normal -> normal offset,
rem written to RGBA32F atlas x2-frame ping-pong (Q10-2). Probe dots viz proves
rem coverage + near-dense/far-sparse. Assertions:
rem   - both backends render probe-place mode exit 0, deviceErrors=0
rem   - re-shot == frozen baseline (deterministic, no regression)
rem   - log anchor: probe cascades=4 base=16 grid 16/8/4/2 probes=340
rem   - dx12 vs vulkan agree within tolerance (check-s9shadow.py near 0.01)
rem   - dot coverage: red/green/blue present and shrinking per level (L3 2x2
rem     grid not asserted - can legitimately land all-sky at this camera)
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4"
set FAIL=0

echo [shot] re-shoot probe-place on both backends (dx12 log captured to verify-log.txt for anchor checks)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode probe-place %ARGS% --screenshot "%OUT%verify-place-%%B.png" > "%OUT%verify-log.txt" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode probe-place %ARGS% --screenshot "%OUT%verify-place-%%B.png" >nul
  )
  if errorlevel 1 (
    echo [FAIL] probe-place-%%B viewer exited nonzero
    set FAIL=1
  )
)
findstr /C:"deviceErrors=0" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] dx12 re-shot reported device errors
  set FAIL=1
) else (
  echo [OK] dx12 re-shot deviceErrors=0
)

echo [check] bytes: re-shot vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%place-%%B.png" "%OUT%verify-place-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] probe-place-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] probe-place-%%B bytes identical to baseline
  )
)

echo [check] log anchor: cascade structure (from dx12 re-shot log)
findstr /C:"probe: cascades=4 base=16 grid=16/8/4/2 probes=340" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] probe cascade log anchor missing
  set FAIL=1
) else (
  echo [OK] probe cascade log anchor present
)

echo [check] dx12 vs vulkan within tolerance
python "%ROOT%\tools\check-s9shadow.py" near "%OUT%verify-place-dx12.png" "%OUT%verify-place-vulkan.png" 0.01
if errorlevel 1 (
  echo [FAIL] dx12/vulkan near gate failed
  set FAIL=1
) else (
  echo [OK] dx12/vulkan near gate passed
)

echo [check] probe dot coverage + near-dense far-sparse ordering
python "%ROOT%\tools\check-s10-place.py" dots "%OUT%verify-place-dx12.png" 300 100 30
if errorlevel 1 (
  echo [FAIL] probe dot coverage gate failed
  set FAIL=1
) else (
  echo [OK] probe dot coverage gate passed
)

echo [check] negative: missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode probe-place --model NoSuchModel.nmesh --frames 8 --no-hud --size 320x240 > "%OUT%verify-negative.txt" 2>&1
if errorlevel 1 (
  echo [OK] negative exited nonzero
) else (
  echo [FAIL] negative silently accepted
  set FAIL=1
)

echo FINAL_FAIL=%FAIL%
if "%FAIL%"=="1" (
  echo VERIFY FAILED
  exit /b 1
)
echo VERIFY OK
exit /b 0

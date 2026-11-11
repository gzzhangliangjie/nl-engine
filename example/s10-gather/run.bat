@echo off
setlocal
rem M10.2 / S10 - final gather acceptance shots.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M10.2 --mode probe-viz --probe-gather: per-pixel final gather over the
rem screen-probe octahedral irradiance atlases (Q13-1 depth-banded cascade
rem level on LINEAR depth; UE bilinear corner x plane-depth weights with
rem fallback degradation, Q13-2 ambient replaced by gather via g_options.y=1).
rem Assertions:
rem   - both backends render gather mode exit 0, deviceErrors=0
rem   - re-shot == frozen baseline (deterministic, no regression)
rem   - log anchors: probe cascade placement + "gather active"
rem   - dx12 vs vulkan agree within tolerance (check-s9shadow.py near 0.01)
rem   - model-region gate: coverage + GI lift p5 + no dead black
rem     (check-s10-gather.py bbox; gate calibrated 2026-11-11 after the 0.03 floor call: p5>=18 dead<0.02)
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --probe-gather"
set FAIL=0

echo [shot] re-shoot probe-viz gather on both backends (dx12 log captured to verify-log.txt for anchor checks)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode probe-viz %ARGS% --screenshot "%OUT%verify-gather-%%B.png" > "%OUT%verify-log.txt" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode probe-viz %ARGS% --screenshot "%OUT%verify-gather-%%B.png" >nul
  )
  if errorlevel 1 (
    echo [FAIL] gather-%%B viewer exited nonzero
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
  fc /b "%OUT%gather-%%B.png" "%OUT%verify-gather-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] gather-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] gather-%%B bytes identical to baseline
  )
)

echo [check] log anchors: cascade structure + gather active (from dx12 re-shot log)
findstr /C:"probe: cascades=4 base=16 grid=16/8/4/2 probes=340" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] probe placement log anchor missing
  set FAIL=1
) else (
  echo [OK] probe placement log anchor present
)
findstr /C:"probe: gather active" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] gather log anchor missing
  set FAIL=1
) else (
  echo [OK] gather log anchor present
)

echo [check] cross-backend agreement (near 0.01 frac)
python "%ROOT%\tools\check-s9shadow.py" near "%OUT%gather-dx12.png" "%OUT%gather-vulkan.png" 0.01
if errorlevel 1 (
  echo [FAIL] backends disagree beyond tolerance
  set FAIL=1
)

echo [check] model-region gate: coverage + GI lift p5 + no dead black
python "%ROOT%\tools\check-s10-gather.py" bbox "%OUT%gather-dx12.png" 50000 18 0.02
if errorlevel 1 (
  echo [FAIL] gather bbox gate failed on dx12
  set FAIL=1
)
python "%ROOT%\tools\check-s10-gather.py" bbox "%OUT%gather-vulkan.png" 50000 18 0.02
if errorlevel 1 (
  echo [FAIL] gather bbox gate failed on vulkan
  set FAIL=1
)

echo [negative] missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode probe-viz --model missing.nmesh --frames 1 --no-hud --probe-gather --screenshot "%OUT%verify-negative.png" >nul
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

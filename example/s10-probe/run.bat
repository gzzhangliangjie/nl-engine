@echo off
setlocal
rem M10.1 / S10 - probe irradiance tracing acceptance shots.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M10.1 --mode probe-viz: per-probe-per-texel direction rays (Q12-1/Q12-2 UE
rem style: 8x8 core + 1 texel border = 10x10 stored texels/probe, 64 rays/probe),
rem miss -> analytic sky gradient (Q12-3), hit -> hit-point direct light via one
rem shadow ray (M9.1 style degraded: uniform albedo, no NdotL). Dots color =
rem octahedral irradiance sampled along probe normal (probeDepth 3-tap rebuild).
rem Assertions:
rem   - both backends render probe-viz mode exit 0, deviceErrors=0
rem   - re-shot == frozen baseline (deterministic, no regression)
rem   - log anchor: probe cascades=4 base=16 grid 16/8/4/2 probes=340 oct=8
rem     raysPerProbe=64
rem   - dx12 vs vulkan agree within tolerance (check-s9shadow.py near 0.01;
rem     M10.1 trace-ray boundary allows a few 1-LSB pixels, base measured 39)
rem   - dot color: skyBlue probe dots present (>=300), zero red level-color
rem     residue (check-s10-probe.py dots)
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7"
set FAIL=0

echo [shot] re-shoot probe-viz on both backends (dx12 log captured to verify-log.txt for anchor checks)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode probe-viz %ARGS% --screenshot "%OUT%verify-probe-%%B.png" > "%OUT%verify-log.txt" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode probe-viz %ARGS% --screenshot "%OUT%verify-probe-%%B.png" >nul
  )
  if errorlevel 1 (
    echo [FAIL] probe-viz-%%B viewer exited nonzero
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
  fc /b "%OUT%probe-%%B.png" "%OUT%verify-probe-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] probe-viz-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] probe-viz-%%B bytes identical to baseline
  )
)

echo [check] log anchor: cascade structure + trace budget (from dx12 re-shot log)
findstr /C:"probe: cascades=4 base=16 grid=16/8/4/2 probes=340 oct=8 raysPerProbe=64" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] probe trace log anchor missing
  set FAIL=1
) else (
  echo [OK] probe trace log anchor present
)

echo [check] cross-backend agreement (near 0.01 frac)
python "%ROOT%\tools\check-s9shadow.py" near "%OUT%probe-dx12.png" "%OUT%probe-vulkan.png" 0.01
if errorlevel 1 (
  echo [FAIL] backends disagree beyond tolerance
  set FAIL=1
)

echo [check] dot color gate: skyBlue irradiance dots + zero level-color residue
python "%ROOT%\tools\check-s10-probe.py" dots "%OUT%probe-dx12.png" 300 0
if errorlevel 1 (
  echo [FAIL] probe dot color gate failed
  set FAIL=1
)
python "%ROOT%\tools\check-s10-probe.py" dots "%OUT%probe-vulkan.png" 300 0
if errorlevel 1 (
  echo [FAIL] probe dot color gate failed on vulkan
  set FAIL=1
)

echo [negative] missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode probe-viz --model missing.nmesh --frames 1 --no-hud --screenshot "%OUT%verify-negative.png" >nul
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

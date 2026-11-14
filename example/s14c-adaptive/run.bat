@echo off
setlocal
rem M14C1+M14C2 (S14-C) adaptive-probe baseline + deterministic re-shot acceptance.
rem ASCII only; CRLF; no parentheses inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem probe chain is deterministic (Q-B1 nondeterminism is restir-only; the
rem adaptive Mark uses a fixed 16x16 Hammersley grid + InterlockedMin, Spawn
rem writes per-tile slots deterministically) so re-shot gates are bytes
rem (fc /b), same as s10-probe.
rem
rem Acceptance gates (PLAN section 9, M14C2 re-scope 2026-11-14):
rem   - adaptive re-shot on both backends exits 0; dx12 deviceErrors=0
rem   - log anchors: probe-adaptive: active (res_eff=8 ...) + shutdown ok
rem   - bytes gate: spawn-on re-shot vs frozen baseline (both backends)
rem   - CONSERVATION gate (replaces the M14C1 adaptive-vs-u8 bit-exact gate,
rem     which the M14C2 spacing-gate fallback breaks BY DESIGN when the spawn
rem     pool activates at L2/L3): adaptive --probe-adaptive-nospawn vs uniform
rem     probe --probe-base-res 8, same effective grid, fc /b identical on both
rem     backends. Proves the uniform interpolation path + 213.8 guard stay
rem     bit-frozen with the spawn pool switched off.
rem   - ACTIVATION gate: adaptive spawn-on vs adaptive nospawn MUST differ
rem     (fc /b not identical, both backends) - mechanical proof that the
rem     M14C2 spacing fallback actually feeds the spawn pool. Gain is MIXED
rem     in this scene (detail-region MSE vs u32 improves up to 61%, silhouette
rem     blocks worsen from sky-miss texel contamination); numbers recorded in
rem     PLAN section 9, quality fix filed as follow-up.
rem   - timing gate: csTraceProbe adaptive vs uniform-8 probe, fresh capture
rem     in this run, ratio <= 1.5 (structural conservation + noise headroom)
rem   - viz gate: magenta spawn dots present (both backends)
rem   - present gate: baselines not degenerate
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7"
set FAIL=0

echo [shot] re-shoot adaptive spawn-on on both backends (dx12 log captured to verify-log.txt, doubles as adaptive timing capture)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode probe-adaptive --pass-timing %ARGS% --screenshot "%OUT%verify-adaptive-%%B.png" > "%OUT%verify-log.txt" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode probe-adaptive %ARGS% --screenshot "%OUT%verify-adaptive-%%B.png" >nul 2>&1
  )
  if errorlevel 1 (
    echo [FAIL] adaptive-%%B viewer exited nonzero
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
findstr /C:"probe-adaptive: active" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] probe-adaptive active anchor missing
  set FAIL=1
) else (
  echo [OK] probe-adaptive: active anchor present
)
findstr /C:"shutdown ok" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] shutdown ok anchor missing
  set FAIL=1
) else (
  echo [OK] shutdown ok anchor present
)

echo [shot] re-shoot adaptive nospawn on both backends (conservation-gate input)
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode probe-adaptive --probe-adaptive-nospawn %ARGS% --screenshot "%OUT%verify-adaptive-nospawn-%%B.png" >nul 2>&1
  if errorlevel 1 (
    echo [FAIL] adaptive-nospawn-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [shot] re-shoot uniform-8 ref on both backends (dx12 log captured to timing-probe.log, doubles as probe timing capture)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode probe --probe-base-res 8 --pass-timing %ARGS% --screenshot "%OUT%verify-ref-u8-%%B.png" > "%OUT%timing-probe.log" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode probe --probe-base-res 8 %ARGS% --screenshot "%OUT%verify-ref-u8-%%B.png" >nul 2>&1
  )
  if errorlevel 1 (
    echo [FAIL] ref-u8-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] bytes: spawn-on re-shot vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%adaptive-%%B.png" "%OUT%verify-adaptive-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] adaptive-%%B bytes mismatch vs frozen baseline
    set FAIL=1
  ) else (
    echo [OK] adaptive-%%B bytes identical
  )
)

echo [check] conservation: adaptive nospawn vs uniform-8 same grid, bit-identical
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-adaptive-nospawn-%%B.png" "%OUT%verify-ref-u8-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] adaptive-nospawn-%%B not bit-identical to uniform-8
    set FAIL=1
  ) else (
    echo [OK] adaptive-nospawn-%%B bit-identical to uniform-8
  )
)

echo [check] activation: adaptive spawn-on MUST differ from nospawn
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-adaptive-%%B.png" "%OUT%verify-adaptive-nospawn-%%B.png" >nul 2>&1
  if errorlevel 1 (
    echo [OK] adaptive-%%B spawn pool activated, output differs from nospawn
  ) else (
    echo [FAIL] adaptive-%%B spawn pool did not activate
    set FAIL=1
  )
)

echo [check] timing: csTraceProbe conservation, fresh capture
python "%ROOT%\tools\check-s14c-adaptive.py" timing "%OUT%timing-probe.log" "%OUT%verify-log.txt" 1.5
if errorlevel 1 (
  echo [FAIL] trace timing conservation gate
  set FAIL=1
)

echo [check] viz: magenta spawn dots present
python "%ROOT%\tools\check-s14c-adaptive.py" magenta "%OUT%adaptive-viz-dx12.png" 100
if errorlevel 1 (
  echo [FAIL] adaptive viz magenta dots gate dx12
  set FAIL=1
)
python "%ROOT%\tools\check-s14c-adaptive.py" magenta "%OUT%adaptive-viz-vulkan.png" 100
if errorlevel 1 (
  echo [FAIL] adaptive viz magenta dots gate vulkan
  set FAIL=1
)

echo [check] present gate: baselines not degenerate
python "%ROOT%\tools\check-s14c-adaptive.py" present "%OUT%adaptive-dx12.png" 2.0 10
if errorlevel 1 (
  echo [FAIL] dx12 baseline present gate failed
  set FAIL=1
)
python "%ROOT%\tools\check-s14c-adaptive.py" present "%OUT%adaptive-vulkan.png" 2.0 10
if errorlevel 1 (
  echo [FAIL] vulkan baseline present gate failed
  set FAIL=1
)

echo [negative] missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode probe-adaptive --model missing.nmesh --frames 1 --no-hud --screenshot "%OUT%verify-negative.png" >nul 2>&1
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

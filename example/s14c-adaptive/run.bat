@echo off
setlocal
rem M14C1 (S14-C) adaptive-probe baseline + deterministic re-shot acceptance.
rem ASCII only; CRLF; no parentheses inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem probe chain is deterministic (Q-B1 nondeterminism is restir-only; the
rem adaptive Mark uses a fixed 16x16 Hammersley grid + InterlockedMin, Spawn
rem writes per-tile slots deterministically) so re-shot gates are bytes
rem (fc /b), same as s10-probe.
rem
rem Acceptance gates (PLAN section 9, 2026-11-14):
rem   - adaptive re-shot on both backends exits 0; dx12 deviceErrors=0
rem   - log anchors: probe-adaptive: active (res_eff=8 ...) + shutdown ok
rem   - bytes gate: re-shot vs frozen baseline (both backends)
rem   - BIT-EXACT gate (replaces the retired MSE/u32 reference gate):
rem     adaptive default base 16 (res_eff=8) vs uniform probe
rem     --probe-base-res 8, same effective grid, fc /b on both backends.
rem     After the 213.8 root fix (L3 reserved-region leak, commit cc69c7d)
rem     adaptive and uniform-8 are bit-identical; spawn GI contribution is
rem     measured 0 in this scene (mechanism kept; sparse-grid acceptance
rem     scene filed as a follow-up in PLAN section 9).
rem   - timing gate: csTraceProbe adaptive vs uniform-8 probe, fresh capture
rem     in this run, ratio <= 1.5 (structural conservation + noise headroom)
rem   - viz gate: magenta spawn dots present (both backends)
rem   - present gate: baselines not degenerate
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7"
set FAIL=0

echo [shot] re-shoot adaptive on both backends (dx12 log captured to verify-log.txt, doubles as adaptive timing capture)
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

echo [check] bytes: re-shot vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%adaptive-%%B.png" "%OUT%verify-adaptive-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] adaptive-%%B bytes mismatch vs frozen baseline
    set FAIL=1
  ) else (
    echo [OK] adaptive-%%B bytes identical
  )
)

echo [check] bit-exact: adaptive vs uniform-8 same grid (213.8 root fix gate)
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-adaptive-%%B.png" "%OUT%verify-ref-u8-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] adaptive-%%B not bit-identical to uniform-8
    set FAIL=1
  ) else (
    echo [OK] adaptive-%%B bit-identical to uniform-8
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

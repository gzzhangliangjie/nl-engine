@echo off
setlocal
rem M14C1+M14C2+M14C3 (S14-C) adaptive-probe baseline + deterministic re-shot acceptance.
rem ASCII only; CRLF; no parentheses inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem probe chain is deterministic (Q-B1 nondeterminism is restir-only; the
rem adaptive Mark uses a fixed 16x16 Hammersley grid + InterlockedMin, Spawn
rem writes per-tile slots deterministically, sky fixup threshold is a fixed
rem constant) so re-shot gates are bytes (fc /b), same as s10-probe.
rem
rem Acceptance gates (PLAN section 9 M14C2 re-scope + section 11 M14C3 2026-11-14):
rem   - adaptive re-shot on both backends exits 0; dx12 deviceErrors=0
rem   - log anchors: probe-adaptive: active (res_eff=8 ...) + shutdown ok
rem   - bytes gate: spawn-on re-shot (sky fixup ON, new default) vs M14C3
rem     frozen baseline (both backends)
rem   - NOFIX-FREEZE gate (M14C3): adaptive --probe-sky-fixup-off re-shot vs
rem     archived M14C2 baseline adaptive-m14c2-*.png, fc /b identical on both
rem     backends. Proves the fixup pass + trace a-mark changes are fully
rem     gated off: with the flag on, bytes match the M14C2 behavior exactly
rem     (irrad alpha channel is write-only for consumers, so the a-mark edit
rem     alone must not move any pixel).
rem   - FIXUP-EFFECT gate (M14C3): fixup-on re-shot vs fixup-off re-shot MUST
rem     differ (fc /b not identical, both backends) - mechanical proof that
rem     the sky quality gate actually culls silhouette spawn probes.
rem   - CONSERVATION gate (M14C2): adaptive --probe-adaptive-nospawn vs uniform
rem     probe --probe-base-res 8, same effective grid, fc /b identical on both
rem     backends (fixup does not dispatch with the spawn pool off).
rem   - ACTIVATION gate (M14C2): adaptive spawn-on vs adaptive nospawn MUST
rem     differ (both backends) - spawn pool feeds the gather.
rem   - timing gate: csTraceProbe adaptive vs uniform-8 probe, fresh capture
rem     in this run, ratio <= 1.5
rem   - viz gate: magenta spawn dots present (both backends; count drops vs
rem     M14C2 because culled silhouette probes no longer draw - expected)
rem   - present gate: baselines not degenerate
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7"
set FAIL=0

echo [shot] re-shoot adaptive spawn-on sky-fixup-on on both backends (dx12 log captured to verify-log.txt, doubles as adaptive timing capture)
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

echo [shot] re-shoot adaptive sky-fixup-off on both backends (nofix-freeze-gate input)
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode probe-adaptive --probe-sky-fixup-off %ARGS% --screenshot "%OUT%verify-adaptive-nofix-%%B.png" >nul 2>&1
  if errorlevel 1 (
    echo [FAIL] adaptive-nofix-%%B viewer exited nonzero
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

echo [check] bytes: spawn-on re-shot vs M14C3 frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%adaptive-%%B.png" "%OUT%verify-adaptive-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] adaptive-%%B bytes mismatch vs frozen baseline
    set FAIL=1
  ) else (
    echo [OK] adaptive-%%B bytes identical
  )
)

echo [check] nofix-freeze: sky-fixup-off re-shot vs archived M14C2 baseline, bit-identical
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%adaptive-m14c2-%%B.png" "%OUT%verify-adaptive-nofix-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] adaptive-nofix-%%B not bit-identical to M14C2 baseline
    set FAIL=1
  ) else (
    echo [OK] adaptive-nofix-%%B bit-identical to M14C2 baseline
  )
)

echo [check] fixup-effect: fixup-on re-shot MUST differ from fixup-off re-shot
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-adaptive-%%B.png" "%OUT%verify-adaptive-nofix-%%B.png" >nul 2>&1
  if errorlevel 1 (
    echo [OK] adaptive-%%B sky fixup active, output differs from fixup-off
  ) else (
    echo [FAIL] adaptive-%%B sky fixup had no effect
    set FAIL=1
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

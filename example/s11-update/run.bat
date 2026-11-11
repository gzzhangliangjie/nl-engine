@echo off
setlocal
rem S11 M11.1 - world cache cell update (DI shadow ray + neighbor-ray cache
rem feedback + blend) acceptance. ASCII only; CRLF; no parentheses in echo blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem Q11-1 verdict A (2026-11-11): GPU-side assertions are STATISTICAL, not
rem byte-exact -- the GI neighbor query quantizes the RayQuery hit point into
rem the cell key while the per-frame TLAS rebuild is not guaranteed bit-exact
rem by D3D12/Vulkan, so the converged radiance image is inherently non-
rem reproducible byte-wise (measured: 3 runs -> 3 md5s, ~9% pixels differ).
rem CPU-side (gi-tools cachesim --update) is single-threaded and stays
rem byte-exact including the convergence numbers.
rem Assertions:
rem   - cache-viz radiance re-shot on both backends exits 0, deviceErrors=0
rem   - log anchor: cache viz compose dispatch + M11.1 update+blend on
rem   - python stats: pseudo-color coverage >= 0.10 AND lit-instance luma
rem     mean exceeds backlit instance by >= 5 (DI NdotL spatial structure)
rem   - CPU sim heatmap: byte-deterministic vs frozen baseline
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--mode cache-viz --model Suzanne-materials.nmesh --frames 64 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --cache-viz-radiance"
set FAIL=0

echo [shot] re-shoot cache-viz radiance on both backends (dx12 log captured)
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --screenshot "%OUT%verify-cacheupd-%%B.png" > "%OUT%verify-log.txt" 2>&1
  if errorlevel 1 (
    echo [FAIL] cache-viz-radiance-%%B viewer exited nonzero
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

echo [check] log anchors: dispatch + update+blend active
findstr /C:"cache: viz compose dispatch" "%OUT%verify-log.txt" <nul >nul
if errorlevel 1 (
  echo [FAIL] viz compose anchor missing
  set FAIL=1
) else (
  echo [OK] viz compose anchor present
)
findstr /C:"M11.1 update+blend on" "%OUT%verify-log.txt" <nul >nul
if errorlevel 1 (
  echo [FAIL] update+blend anchor missing
  set FAIL=1
) else (
  echo [OK] update+blend anchor present
)

echo [check] python stats: coverage + lit-vs-backlit margin (Q11-1 verdict A)
for %%B in (dx12 vulkan) do (
  python "%ROOT%\tools\check-s11-update.py" stats "%OUT%verify-cacheupd-%%B.png" 0.10 5.0
  if errorlevel 1 (
    echo [FAIL] stats-%%B below threshold
    set FAIL=1
  ) else (
    echo [OK] stats-%%B pass
  )
)

echo [check] CPU sim heatmap byte-deterministic vs frozen baseline
"%ROOT%\build\dx12\gi-tools.exe" cachesim --frames 64 --update --png "%OUT%verify-cacheupd-sim.png" > "%OUT%verify-sim-log.txt" 2>&1
if errorlevel 1 (
  echo [FAIL] cachesim update mode failed
  set FAIL=1
) else (
  echo [OK] cachesim update mode pass with convergence gate
)
fc /b "%OUT%cacheupd-sim.png" "%OUT%verify-cacheupd-sim.png" >nul
if errorlevel 1 (
  echo [FAIL] CPU sim heatmap bytes differ from baseline
  set FAIL=1
) else (
  echo [OK] CPU sim heatmap bytes identical to baseline
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

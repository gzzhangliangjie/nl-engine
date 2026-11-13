@echo off
setlocal
rem M13.0 (S13) per-cluster BLAS prebuild acceptance shots.
rem ASCII only.
rem Usage: run.bat verify   (re-shots pairs + cluster-blas log evidence, exit 0 = ok)
rem
rem Evidence chain:
rem   - dag mode on Suzanne chain -> initDag flatten loop done -> initClusterBlas
rem     expands per-cluster build-input slabs (pos-only 12B VB + u8->u32 IB),
rem     one AccelStruct per cluster referencing shared slab windows
rem     (GeometryTriangles.vertexOffset/indexOffset), all builds recorded on one
rem     command list (M8.0 0x87d discipline: upload + setBufferState + build +
rem     setPermanentBufferState same list + waitForIdle), GPU-timed.
rem   - Machine-readable evidence: "cluster-blas: built N blas | tri=T |
rem     inputBytes=B | estBytes=E" (deterministic geometry => identical numbers
rem     on both backends and across runs).
rem   - OcclScene1M scale probe: 43547 blas / 2613978 tri, ~190MB est, proves
rem     ten-thousand-scale BLAS build succeeds (plan's memory pitfall logged).
rem   - HUD evidence line "CLAS : N blas ..." (deterministic screenshots hide ms;
rem     only shown with --cluster-blas, other modes untouched pixel-for-pixel).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] clusterblas-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode dag --hud --cluster-blas --screenshot "%OUT%clusterblas-hud-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 set FAIL=1
)
echo [shot] clusterblas-occl pair (scale probe, ~40s per backend) ...
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode dag --model OcclScene1M.nmesh --hud --cluster-blas --screenshot "%OUT%clusterblas-occl-%%B.png" --frames 8 --size 320x240
  if errorlevel 1 set FAIL=1
)
echo [check] dx12: cluster-blas built info line ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --cluster-blas --screenshot "%OUT%verify-log-dx12.png" --frames 8 --size 320x240 | findstr /C:"cluster-blas: built 324 blas | tri=7906 | inputBytes=277764 | estBytes=671872"
if errorlevel 1 set FAIL=1
echo [check] vulkan: cluster-blas built info line ...
"%ROOT%\build\vulkan\viewer.exe" --mode dag --cluster-blas --screenshot "%OUT%verify-log-vulkan.png" --frames 8 --size 320x240 | findstr /C:"cluster-blas: built 324 blas | tri=7906 | inputBytes=277764 | estBytes=671872"
if errorlevel 1 set FAIL=1
echo [check] dx12: one-shot clusterblas GPU timer resolve ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --hud --cluster-blas --screenshot "%OUT%verify-timer-dx12.png" --frames 8 --size 320x240 | findstr /C:"gpu timer resolved [clusterblas]"
if errorlevel 1 set FAIL=1
echo [check] dx12: OcclScene1M scale anchor (43547 blas) ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --model OcclScene1M.nmesh --cluster-blas --screenshot "%OUT%verify-occl-dx12.png" --frames 8 --size 320x240 | findstr /C:"cluster-blas: built 43547 blas | tri=2613978"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s13-preblas verify
  exit /b 1
)
echo [OK] s13-preblas verify: pairs + built/timer/scale log checks
exit /b 0

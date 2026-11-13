@echo off
setlocal
rem M13.1 (S13) visible-cluster-list readback + cluster TLAS acceptance shots.
rem ASCII only.
rem Usage: run.bat verify   (re-shots pairs + cluster-tlas log evidence, exit 0 = ok)
rem
rem Evidence chain (plan A: TLAS directly references per-visible-cluster BLAS):
rem   - rt mode (frustum cull always off -> no dispatch -> readback never fills):
rem     TLAS instances = full cluster identity list = "all clusters drawn" cut
rem     semantics; --rt-debug id pseudocolor shows per-cluster hues (fine
rem     geometry traced, not the fallback proxy).
rem   - dag mode (S5 traversal dispatch every frame): visible cluster list is
rem     copied whole into a 2-slot host-visible ring each frame (tail 4B =
rem     args[1] count); pollClusterVisible consumes it 2 frames later
rem     (pollCullStats window discipline, zero-wait maps). TLAS rebuilds per
rem     frame from the readback list, instanceID = global cluster id.
rem   - Machine-readable anchors (deterministic camera -> geometry-decided
rem     counts, identical on both backends):
rem       "cluster-tlas: per-frame rebuild active | instances=324 (full cluster
rem        list -- readback not ready)"          (rt mode, frame 1)
rem       "cluster-tlas: 237 instances (source=readback visible list frame 0)"
rem                                             (dag mode, frame 3, yaw12/p9/d4.2)
rem       "cluster-tlas: per-frame rebuild active | instances=43547"
rem                                             (OcclScene1M scale probe)
rem   - OcclScene1M: 43547 TLAS instances rebuilt every frame, deviceErrors=0
rem     (TLAS build cost at 43k instances stays off the TDR radar).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

echo [shot] rt-id pair (per-cluster pseudocolor via cluster TLAS) ...
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode rt --cluster-blas --rt-debug id --screenshot "%OUT%clustertlas-rt-id-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 set FAIL=1
)
echo [shot] dag-tlas-hud pair (HUD TLAS line = readback instance count) ...
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode dag --hud --cluster-blas --screenshot "%OUT%clustertlas-dag-hud-%%B.png" --frames 12 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 set FAIL=1
)
echo [check] dx12: rt mode full-cluster-list TLAS anchor ...
"%ROOT%\build\dx12\viewer.exe" --mode rt --cluster-blas --screenshot "%OUT%verify-rt-full-dx12.png" --frames 8 --size 320x240 | findstr /C:"cluster-tlas: per-frame rebuild active | instances=324"
if errorlevel 1 set FAIL=1
echo [check] dx12: dag mode readback visible-list TLAS anchor ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --cluster-blas --screenshot "%OUT%verify-dag-rb-dx12.png" --frames 12 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"cluster-tlas: 237 instances (source=readback visible list frame 0)"
if errorlevel 1 set FAIL=1
echo [check] vulkan: dag mode readback visible-list TLAS anchor ...
"%ROOT%\build\vulkan\viewer.exe" --mode dag --cluster-blas --screenshot "%OUT%verify-dag-rb-vulkan.png" --frames 12 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 | findstr /C:"cluster-tlas: 237 instances (source=readback visible list frame 0)"
if errorlevel 1 set FAIL=1
echo [check] dx12: OcclScene1M scale anchor (43547 TLAS instances per frame) ...
"%ROOT%\build\dx12\viewer.exe" --mode rt --model OcclScene1M.nmesh --cluster-blas --screenshot "%OUT%verify-occl-rt-dx12.png" --frames 8 --size 320x240 | findstr /C:"cluster-tlas: per-frame rebuild active | instances=43547"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s13-merge verify
  exit /b 1
)
echo [OK] s13-merge verify: pairs + full-list/readback/scale log checks
exit /b 0

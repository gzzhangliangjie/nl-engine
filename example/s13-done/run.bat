@echo off
setlocal
rem M13.2 (S13) seam acceptance: GI on current-cut geometry + LOD-switch stress.
rem ASCII only.
rem Usage: run.bat verify   (re-shots pairs + log anchors, exit 0 = ok)
rem
rem Evidence chain:
rem   - gi-cluster (filter mode = S12 GI chain, probe_trace/world_cache/direct all
rem     bind the same tlas_): --cluster-blas swaps in per-cluster BLAS instances,
rem     GI rays trace fine cluster geometry (UE r.RayTracing.Nanite.LODBias=0
rem     semantics: RT geometry is never coarser than the raster cut).
rem     Anchor: "cluster-tlas: per-frame rebuild active | instances=711"
rem     (Suzanne-materials chain, 711 blas / 11808 tri, byte-identical both
rem     backends).
rem   - lod-pressure (dag mode, --cut-thr 16 + --orbit-dist-speed 0.08, dist
rem     3.0 -> 10.2 over 90 frames crosses the cut error boundary): S5 cut
rem     walks 3936 -> 2632 tris, readback visible list follows, TLAS instance
rem     count tracks the cut every frame (237 -> 77). GPU frame time stays
rem     0.63 ms flat (no stall spike), deviceErrors=0 across the run.
rem     Anchor: "cluster-tlas: 237 instances (source=readback visible list
rem     frame 0)" (frame 3, yaw12/p9/d3.0 -- byte-identical both backends).
rem   - Sequence frames archived at docs/screenshots/s13_lod_seq_f*_dx12.png
rem     (f10 main layer / f35 mid / f60 coarser / f85 coarsest).
rem   - Nsight/RGP not available in this environment (sandbox, no GUI); stall
rem     evidence = M-PERF1 --pass-timing anchors (csCull 0.15 ms, mainDraw
rem     0.012 ms) + flat 0.63 ms GPU frame time across the 90-frame run +
rem     zero-wait mapBuffer window discipline (pollClusterVisible = submitted-2).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

echo [shot] gi-cluster pair (filter GI chain on cluster TLAS) ...
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode filter --filter-gi-dump --cluster-blas --model Suzanne-materials.nmesh --frames 16 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4 --screenshot "%OUT%gi-cluster-%%B.png"
  if errorlevel 1 set FAIL=1
)
echo [shot] lod-pressure pair (90-frame dist animation crossing cut boundary) ...
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode dag --hud --cluster-blas --cut-thr 16 --orbit-dist-speed 0.08 --screenshot "%OUT%lod-pressure-%%B.png" --frames 90 --yaw 12 --pitch 9 --dist 3.0 --size 960x540
  if errorlevel 1 set FAIL=1
)
echo [check] dx12: filter GI chain on cluster TLAS anchor (711 instances) ...
"%ROOT%\build\dx12\viewer.exe" --mode filter --cluster-blas --model Suzanne-materials.nmesh --screenshot "%OUT%verify-gi-anchor-dx12.png" --frames 16 --size 320x240 | findstr /C:"cluster-tlas: per-frame rebuild active | instances=711"
if errorlevel 1 set FAIL=1
echo [check] vulkan: filter GI chain on cluster TLAS anchor (711 instances) ...
"%ROOT%\build\vulkan\viewer.exe" --mode filter --cluster-blas --model Suzanne-materials.nmesh --screenshot "%OUT%verify-gi-anchor-vulkan.png" --frames 16 --size 320x240 | findstr /C:"cluster-tlas: per-frame rebuild active | instances=711"
if errorlevel 1 set FAIL=1
echo [check] dx12: lod-pressure readback anchor ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --cluster-blas --cut-thr 16 --screenshot "%OUT%verify-lod-anchor-dx12.png" --frames 12 --yaw 12 --pitch 9 --dist 3.0 --size 960x540 | findstr /C:"cluster-tlas: 237 instances (source=readback visible list frame 0)"
if errorlevel 1 set FAIL=1
echo [check] vulkan: lod-pressure readback anchor ...
"%ROOT%\build\vulkan\viewer.exe" --mode dag --cluster-blas --cut-thr 16 --screenshot "%OUT%verify-lod-anchor-vulkan.png" --frames 12 --yaw 12 --pitch 9 --dist 3.0 --size 960x540 | findstr /C:"cluster-tlas: 237 instances (source=readback visible list frame 0)"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s13-done verify
  exit /b 1
)
echo [OK] s13-done verify: pairs + gi-anchor/lod-anchor log checks
exit /b 0

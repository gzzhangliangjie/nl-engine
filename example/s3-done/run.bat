@echo off
setlocal
rem ==== S3.2 (M3.2) successful cases ========================================
rem viewer --mode dag-top: renders the DAG top (coarsest layer, the correct
rem coarse model) and --dag-group N drills group-atomically: remove the parent
rem cluster(s) the group generated, insert the group's member clusters from the
rem second-coarsest layer. This is the offline preview of the S5 group-atomic
rem cut. Mixed-set seam integrity is mechanically checked by
rem tools/dag_weld_check.py (no new open edges, no overlap edges).
rem usage: run.bat [all^|gallery^|weld^|selftest]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\dx12\viewer.exe"
set "VK=%ROOT%\build\vulkan\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="gallery" goto gallery
if /i "%CASE%"=="weld" goto weld
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|gallery^|weld^|selftest]
exit /b 1

:all
call :gallery || exit /b 1
call :weld || exit /b 1
call :selftest || exit /b 1
exit /b 0

:gallery
echo === S3.2 dag-top: pure top + three group drills (one camera, real viewer) ===
"%DX%" --mode dag-top --screenshot "%OUT%\s3-done-top-DX12.png" --frames 8 --no-hud --shading normal --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
for %%g in (0 1 2) do (
    "%DX%" --mode dag-top --dag-group %%g --screenshot "%OUT%\s3-done-g%%g-DX12.png" --frames 8 --no-hud --shading normal --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
)
python "%ROOT%\tools\dag_gallery.py" ^
    --tile "top L6 (104 tri):%OUT%\s3-done-top-DX12.png" ^
    --tile "drill g0 (5 children, 110->72 folded):%OUT%\s3-done-g0-DX12.png" ^
    --tile "drill g1 (16->16 no collapse):%OUT%\s3-done-g1-DX12.png" ^
    --tile "drill g2 (16->16 no collapse):%OUT%\s3-done-g2-DX12.png" ^
    --out "%ROOT%\example\s3-done\dag-drills.png" || exit /b 1
echo expected: every drill seals seamlessly with the kept coarse clusters
echo           (no holes / no cracks / no overlaps); the 84 open edges of the
echo           source shell (eyes+mouth) stay identical in every tile.
exit /b 0

:weld
echo === S3.2 offline seam check: welded-edge manifold of every drill ===
python "%ROOT%\tools\dag_weld_check.py" --prefix "%ROOT%\assets\meshlets\Suzanne" --all || exit /b 1
exit /b 0

:selftest
echo === S3.2 selftest (15/15 each backend; stage check now expects S3.2) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: build\_build.bat DX12 / VULKAN
exit /b 1

@echo off
setlocal
rem ==== S2.0 (M2.3) successful cases ========================================
rem viewer --mode lod N: layered .nmesh assets (assets/meshlets/Suzanne.lodN.nmesh,
rem produced by `nanite-tools lod Suzanne.nmesh -o assets/meshlets/Suzanne`).
rem The six-layer gallery is assembled from the real DX12 screenshots below.
rem usage: run.bat [all^|gallery^|selftest]
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
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|gallery^|selftest]
exit /b 1

:all
call :gallery || exit /b 1
call :selftest || exit /b 1
exit /b 0

:gallery
echo === S2.0 six LOD layers through the real viewer (one camera) ===
for %%n in (0 1 2 3 4 5) do (
    "%DX%" --mode lod --lod %%n --model Suzanne.nmesh --screenshot "%OUT%\s2-done-lod-l%%n-DX12.png" --frames 8 --no-hud --yaw 12 --pitch 9 --dist 4.2 --shading normal || exit /b 1
)
python "%ROOT%\tools\lod_gallery.py" --prefix "%ROOT%\assets\meshlets\Suzanne" --pattern "%OUT%\s2-done-lod-l{n}-DX12.png" --out "%ROOT%\example\s2-done\lod-layers.png" || exit /b 1
echo expected: six tiles L0..L5, triangle counts 3936/1976/985/490/246/124, no seams or holes
echo           (L4/L5 show known flipped triangles from M2.2, cull=none proves geometry intact)
exit /b 0

:selftest
echo === S2.0 selftest (15/15 each backend; stage check now expects S2.0) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: build\_build.bat DX12 / VULKAN
exit /b 1

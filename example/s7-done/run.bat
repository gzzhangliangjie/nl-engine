@echo off
setlocal
rem ==== M7.2 (S7.2) visibility buffer multi-material indirect table ===========
rem Three-material asset (Suzanne-materials: polished-metal red / matte green /
rem semi-metal blue). Vis path: raster pass writes the u64 vis buffer, then the
rem single full-screen pass resolves cluster -> clusterMeta[3] materialId ->
rem GPUMaterial indirect table -> NonUniformResourceIndex texture slots.
rem Reference path: traditional direct draw with per-prim batches (model.slang
rem untouched). The two are compared pixel-by-pixel as the S7 acceptance gate.
rem usage: run.bat [all^|pulling^|reference^|verify^|selftest]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\DX12\viewer.exe"
set "VK=%ROOT%\build\VULKAN\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="pulling" goto pulling
if /i "%CASE%"=="reference" goto reference
if /i "%CASE%"=="verify" goto verify
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|pulling^|reference^|verify^|selftest]
exit /b 1

:all
call :pulling || exit /b 1
call :reference || exit /b 1
call :verify || exit /b 1
call :selftest || exit /b 1
exit /b 0

:pulling
echo === M7.2 vis buffer multi-material (PBR direct light, mode 5) ===
"%DX%" --mode pulling --model Suzanne-materials.nmesh --shading 5 --screenshot "%OUT%\s72-pulling-DX12.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 9 || exit /b 1
"%VK%" --mode pulling --model Suzanne-materials.nmesh --shading 5 --screenshot "%OUT%\s72-pulling-VULKAN.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 9 || exit /b 1
echo expected: three Suzannes = polished-metal red / matte green / semi-metal blue
exit /b 0

:reference
echo === M7.2 traditional direct draw reference (per-prim batches, mode 5) ===
"%DX%" --mode normal --model Suzanne-materials.glb --shading 5 --screenshot "%OUT%\s72-normalref-DX12.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 9 || exit /b 1
"%VK%" --mode normal --model Suzanne-materials.glb --shading 5 --screenshot "%OUT%\s72-normalref-VULKAN.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 9 || exit /b 1
echo expected: same three materials as the pulling shot (indirect table vs per-prim sets)
exit /b 0

:verify
echo === M7.2 acceptance verify (baselines + cross-backend + foreground A/B) ===
python "%~dp0verify.py" || exit /b 1
exit /b 0

:selftest
echo === M7.2 selftest (16/16 each backend; stage check expects S7.2) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

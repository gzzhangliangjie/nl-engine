@echo off
setlocal
rem ==== M7.1 (S7.1) vis buffer material resolve (vertex pulling) ==============
rem --mode pulling: raster pass writes the same u64 vis buffer as M7.0, then a
rem single full-screen pass resolves it: unpack -> clusterMeta lookup -> pull
rem 3 vertices -> analytic barycentrics/derivatives -> PBR mode-0 shading.
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
echo === M7.1 vis buffer material resolve (textured PBR, mode 0) ===
"%DX%" --mode pulling --screenshot "%OUT%\s71-pulling-DX12.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
"%VK%" --mode pulling --screenshot "%OUT%\s71-pulling-VULKAN.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
echo expected: Suzanne with baseColor texture + direct light, no cracks / speckle
exit /b 0

:reference
echo === M7.1 traditional direct draw reference (normal mode, textured) ===
"%DX%" --mode normal --shading 0 --screenshot "%OUT%\s71-normalref-DX12.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
"%VK%" --mode normal --shading 0 --screenshot "%OUT%\s71-normalref-VULKAN.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
echo expected: same foreground as the pulling shot (same geometry/light/texture)
exit /b 0

:verify
echo === M7.1 acceptance verify (baselines + cross-backend + foreground A/B) ===
python "%~dp0verify.py" || exit /b 1
exit /b 0

:selftest
echo === M7.1 selftest (16/16 each backend; stage check expects S7.1) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

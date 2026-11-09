@echo off
setlocal
rem ==== M7.0 (S7.0) visibility buffer successful cases =======================
rem --mode visbuf: u64 vis buffer = (asuint(deviceZ)<<32)|(clusterId<<8|triId),
rem written with atomicMin into an RWStructuredBuffer<uint64_t>; a full-screen
rem viz pass decodes clusterId / triangleId to pcg colors.
rem usage: run.bat [all^|cluster^|triangle^|reference^|verify^|selftest]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\DX12\viewer.exe"
set "VK=%ROOT%\build\VULKAN\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="cluster" goto cluster
if /i "%CASE%"=="triangle" goto triangle
if /i "%CASE%"=="reference" goto reference
if /i "%CASE%"=="verify" goto verify
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|cluster^|triangle^|reference^|verify^|selftest]
exit /b 1

:all
call :cluster || exit /b 1
call :triangle || exit /b 1
call :reference || exit /b 1
call :verify || exit /b 1
call :selftest || exit /b 1
exit /b 0

:cluster
echo === M7.0 visbuf clusterId decode (one pcg color per cluster) ===
"%DX%" --mode visbuf --vis-viz cluster --screenshot "%OUT%\s7-cluster-DX12.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
"%VK%" --mode visbuf --vis-viz cluster --screenshot "%OUT%\s7-cluster-VULKAN.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
echo expected: 237 distinct cluster patches, clean edges, correct occlusion
exit /b 0

:triangle
echo === M7.0 visbuf triangleId decode (one pcg color per triangle) ===
"%DX%" --mode visbuf --vis-viz triangle --screenshot "%OUT%\s7-triangle-DX12.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
"%VK%" --mode visbuf --vis-viz triangle --screenshot "%OUT%\s7-triangle-VULKAN.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
echo expected: every triangle a different color, no tearing / speckle
exit /b 0

:reference
echo === M7.0 traditional direct draw reference (cull mode, frustum off) ===
"%DX%" --mode cull --frustum off --shading 4 --screenshot "%OUT%\s7-cullref-DX12.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
"%VK%" --mode cull --frustum off --shading 4 --screenshot "%OUT%\s7-cullref-VULKAN.png" --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4.2 || exit /b 1
echo expected: same foreground as the cluster shot (same pcg hash)
exit /b 0

:verify
echo === M7.0 acceptance verify (baselines + cross-backend + foreground A/B) ===
python "%~dp0verify.py" || exit /b 1
exit /b 0

:selftest
echo === M7.0 selftest (16/16 each backend; stage check expects S7.0) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

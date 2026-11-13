@echo off
rem M-PERF1 (perf-m1) verify: per-pass GPU timing (Q-P4/Q-P5).
rem Tool: tools/check-perf-m1.py (pure-stdlib log anchor checker).
rem Stages per backend (DX12 / VULKAN):
rem   1) checker selftest
rem   2) hiz    ON shot -> anchors csCull / hizBuild / mainDraw + deviceErrors=0
rem   3) filter ON shot -> anchors visRaster / matResolve / csPlace / csTraceProbe
rem                         / csGather / csFilterTemporal / csFilterSpatial
rem                         / csGatherCompose + deviceErrors=0
rem   4) shadow ON shot -> anchors visRaster / csDirect + deviceErrors=0
rem   5) hiz    OFF shot -> zero pass-timing lines (Q-P5 default-off) + deviceErrors=0
rem csRefGI (--mode reference) is exercised by the cases.json card; the four
rem stages above cover every remaining scheduled pass of the Q-P4 list.
setlocal EnableExtensions
set "TOOL=%~dp0..\..\tools\check-perf-m1.py"
set "OUT=%~dp0"

python "%TOOL%" selftest > "%OUT%verify-selftest.log" 2>&1
if errorlevel 1 ( echo [FAIL] checker selftest nonzero exit & exit /b 1 )
findstr /c:"check-perf-m1 selftest: PASS" "%OUT%verify-selftest.log" >nul
if errorlevel 1 ( echo [FAIL] checker selftest anchor missing & exit /b 1 )
echo [OK] checker selftest

call :backend DX12 "%~dp0..\..\build\DX12\viewer.exe"
if errorlevel 1 exit /b 1
call :backend VULKAN "%~dp0..\..\build\VULKAN\viewer.exe"
if errorlevel 1 exit /b 1

echo ALL GREEN [perf-m1]
endlocal
exit /b 0

:backend
set "LABEL=%~1"
set "VIEWER=%~f2"
echo [backend %LABEL%] hiz ON
"%VIEWER%" --mode hiz --pass-timing --frames 16 --no-hud --size 640x720 --yaw 12 --pitch 9 --dist 4.2 --screenshot "%OUT%hiz-%LABEL%.png" > "%OUT%verify-hiz-%LABEL%.log" 2>&1
if errorlevel 1 ( echo [FAIL] %LABEL% hiz shot nonzero exit & exit /b 1 )
python "%TOOL%" anchors "%OUT%verify-hiz-%LABEL%.log" hiz
if errorlevel 1 exit /b 1

echo [backend %LABEL%] filter ON
"%VIEWER%" --mode filter --pass-timing --model Suzanne-materials.nmesh --frames 16 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4 --screenshot "%OUT%filter-%LABEL%.png" > "%OUT%verify-filter-%LABEL%.log" 2>&1
if errorlevel 1 ( echo [FAIL] %LABEL% filter shot nonzero exit & exit /b 1 )
python "%TOOL%" anchors "%OUT%verify-filter-%LABEL%.log" filter
if errorlevel 1 exit /b 1

echo [backend %LABEL%] shadow ON
"%VIEWER%" --mode shadow --pass-timing --frames 16 --no-hud --size 640x720 --yaw 12 --pitch 9 --dist 4.2 --screenshot "%OUT%shadow-%LABEL%.png" > "%OUT%verify-shadow-%LABEL%.log" 2>&1
if errorlevel 1 ( echo [FAIL] %LABEL% shadow shot nonzero exit & exit /b 1 )
python "%TOOL%" anchors "%OUT%verify-shadow-%LABEL%.log" shadow
if errorlevel 1 exit /b 1

echo [backend %LABEL%] hiz OFF (default-off silence)
"%VIEWER%" --mode hiz --frames 16 --no-hud --size 640x720 --yaw 12 --pitch 9 --dist 4.2 --screenshot "%OUT%hiz-off-%LABEL%.png" > "%OUT%verify-hiz-off-%LABEL%.log" 2>&1
if errorlevel 1 ( echo [FAIL] %LABEL% OFF shot nonzero exit & exit /b 1 )
python "%TOOL%" off "%OUT%verify-hiz-off-%LABEL%.log"
if errorlevel 1 exit /b 1

echo [OK] backend %LABEL% complete
exit /b 0

@echo off
setlocal enabledelayedexpansion
rem M-E1b.5 (P8 ECS E1b): parallel tick -- read/write-set solver + jobgraph DAG
rem + structural-change guard. Tool deliverable (A-class style: no GPU, no
rem kStage bump). Three cases: usage (exit 2) / positive selftest (exit 0) /
rem negative unknown suite (exit 2) + evidence PNGs from real logs.
rem M-E1b.5 render cases: viewer --tick serial|parallel drives spin systems on
rem ecs-parallel-spin.scene; serial vs parallel screenshots must be byte-equal
rem (fixed dt=1/60). Static run (tick off) keeps deviceErrors=0 too.
rem Usage: run.bat   (exit 0 = ok)
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).

set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\nanite-scene.exe"
set "VEXE=%ROOT%\build\DX12\viewer.exe"
set "OUT=%~dp0"
set FAIL=0

if not exist "%EXE%" (
  echo [FAIL] missing %EXE%
  exit /b 1
)

echo [case] usage: selftest without --suite must exit 2 ...
"%EXE%" selftest > "%OUT%usage-nosuite.log" 2>&1
if not errorlevel 2 set FAIL=1
findstr /c:"selftest requires --suite" "%OUT%usage-nosuite.log" >nul
if errorlevel 1 set FAIL=1

echo [case] positive: selftest --suite ecs-parallel --verbose must exit 0 ...
"%EXE%" selftest --suite ecs-parallel --verbose > "%OUT%selftest-ecs-parallel.log" 2>&1
if errorlevel 1 set FAIL=1
findstr /c:"ecs-parallel selftest: PASS" "%OUT%selftest-ecs-parallel.log" >nul
if errorlevel 1 set FAIL=1

echo [case] solver dot: parallel-dot real solver output ...
"%EXE%" parallel-dot > "%OUT%parallel-dot.log" 2>&1
if errorlevel 1 set FAIL=1
findstr /c:"digraph jobgraph" "%OUT%parallel-dot.log" >nul
if errorlevel 1 set FAIL=1
findstr /c:"t0 -> t1;" "%OUT%parallel-dot.log" >nul
if errorlevel 1 set FAIL=1

echo [case] negative: unknown suite must exit 2 ...
"%EXE%" selftest --suite no-such-suite > "%OUT%negative-unknown-suite.log" 2>&1
if not errorlevel 2 set FAIL=1
findstr /c:"unknown selftest suite" "%OUT%negative-unknown-suite.log" >nul
if errorlevel 1 set FAIL=1

echo [evidence] regenerate PNGs from real logs ...
python "%ROOT%\scripts\make-ecs-parallel-evidence.py"
if errorlevel 1 set FAIL=1

echo [case] render: spin scene static (tick off) exit 0 + deviceErrors=0 ...
"%VEXE%" --scene "%OUT%ecs-parallel-spin.scene" --screenshot "%OUT%shot-spin-static.png" --frames 4 --size 960x540 --no-hud --yaw 12 --pitch 9 > "%OUT%shot-spin-static.log" 2>&1
if errorlevel 1 set FAIL=1
findstr /c:"deviceErrors=0" "%OUT%shot-spin-static.log" >nul
if errorlevel 1 set FAIL=1
findstr /c:"ecs scene ready" "%OUT%shot-spin-static.log" >nul
if errorlevel 1 set FAIL=1

echo [case] render: tick serial 30 frames exit 0 + deviceErrors=0 ...
"%VEXE%" --scene "%OUT%ecs-parallel-spin.scene" --tick serial --screenshot "%OUT%shot-spin-serial.png" --frames 30 --size 960x540 --no-hud --yaw 12 --pitch 9 > "%OUT%shot-spin-serial.log" 2>&1
if errorlevel 1 set FAIL=1
findstr /c:"deviceErrors=0" "%OUT%shot-spin-serial.log" >nul
if errorlevel 1 set FAIL=1
findstr /c:"ecs tick summary: mode=serial" "%OUT%shot-spin-serial.log" >nul
if errorlevel 1 set FAIL=1

echo [case] render: tick parallel 30 frames exit 0 + deviceErrors=0 ...
"%VEXE%" --scene "%OUT%ecs-parallel-spin.scene" --tick parallel --screenshot "%OUT%shot-spin-parallel.png" --frames 30 --size 960x540 --no-hud --yaw 12 --pitch 9 > "%OUT%shot-spin-parallel.log" 2>&1
if errorlevel 1 set FAIL=1
findstr /c:"deviceErrors=0" "%OUT%shot-spin-parallel.log" >nul
if errorlevel 1 set FAIL=1
findstr /c:"ecs tick summary: mode=parallel" "%OUT%shot-spin-parallel.log" >nul
if errorlevel 1 set FAIL=1

echo [check] serial vs parallel screenshots byte-identical (deterministic) ...
fc /b "%OUT%shot-spin-serial.png" "%OUT%shot-spin-parallel.png" >nul
if errorlevel 1 set FAIL=1

echo [check] animated frame differs from static frame (objects really spin) ...
fc /b "%OUT%shot-spin-static.png" "%OUT%shot-spin-parallel.png" >nul
if not errorlevel 1 set FAIL=1

echo [case] negative: --tick invalid value must exit 1 ...
"%VEXE%" --scene "%OUT%ecs-parallel-spin.scene" --tick warp > "%OUT%negative-tick.log" 2>&1
if not errorlevel 1 set FAIL=1
findstr /c:"invalid --tick" "%OUT%negative-tick.log" >nul
if errorlevel 1 set FAIL=1

if exist "%ROOT%\build\VULKAN\viewer.exe" (
  echo [case] render VULKAN: tick parallel+serial 30 frames + byte-identical ...
  "%ROOT%\build\VULKAN\viewer.exe" --scene "%OUT%ecs-parallel-spin.scene" --tick parallel --screenshot "%OUT%shot-spin-parallel-vk.png" --frames 30 --size 960x540 --no-hud --yaw 12 --pitch 9 > "%OUT%shot-spin-parallel-vk.log" 2>&1
  if errorlevel 1 set FAIL=1
  findstr /c:"deviceErrors=0" "%OUT%shot-spin-parallel-vk.log" >nul
  if errorlevel 1 set FAIL=1
  "%ROOT%\build\VULKAN\viewer.exe" --scene "%OUT%ecs-parallel-spin.scene" --tick serial --screenshot "%OUT%shot-spin-serial-vk.png" --frames 30 --size 960x540 --no-hud --yaw 12 --pitch 9 > "%OUT%shot-spin-serial-vk.log" 2>&1
  if errorlevel 1 set FAIL=1
  findstr /c:"deviceErrors=0" "%OUT%shot-spin-serial-vk.log" >nul
  if errorlevel 1 set FAIL=1
  fc /b "%OUT%shot-spin-serial-vk.png" "%OUT%shot-spin-parallel-vk.png" >nul
  if errorlevel 1 set FAIL=1
) else (
  echo [warn] VULKAN viewer missing, skip VK render chain
)

if not "%FAIL%"=="0" (
  echo [FAIL] M-E1b.5 evidence chain
  exit /b 1
)
echo [PASS] M-E1b.5 evidence regenerated (6 png + 11 logs)
exit /b 0

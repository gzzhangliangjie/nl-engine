@echo off
setlocal enabledelayedexpansion
rem M-E1b.5 (P8 ECS E1b): parallel tick -- read/write-set solver + jobgraph DAG
rem + structural-change guard. Tool deliverable (A-class style: no GPU, no
rem kStage bump). Three cases: usage (exit 2) / positive selftest (exit 0) /
rem negative unknown suite (exit 2) + evidence PNGs from real logs.
rem Usage: run-parallel.bat   (exit 0 = ok)
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).

set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\nanite-scene.exe"
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

if not "%FAIL%"=="0" (
  echo [FAIL] M-E1b.5 evidence chain
  exit /b 1
)
echo [PASS] M-E1b.5 evidence regenerated (2 png + 4 logs)
exit /b 0

@echo off
rem P8 M-J5c acceptance: Jolt JobSystem adapter onto the nl/core ThreadPool
rem (pool-backed parallel physics). Two layers in one gate:
rem   layer 1 (hard proof): adapter N=1 bit-identical to SingleThreaded (drop + stack)
rem   layer 2 (parallel):   adapter N=4 run-to-run deterministic (drop + stack)
rem Plus: adapter N=1 full-file byte-compare against the archived M-P0 drop
rem baseline, and the M-P0..M-P4 5-baseline byte-identical regression.
rem Exit 0 = all green. Build first:
rem   scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"
set "TOOLS=%ROOT%\build\dx12\phys-tools.exe"

if not exist "%TOOLS%" (
    echo [ERROR] phys-tools not built: build\dx12\phys-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest --verbose (M-P0..M-P4 + j5c anchors) ===
"%TOOLS%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] phys-tools selftest FAILED
    exit /b 1
)
findstr /c:"phys-j5c anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-J5c anchor line & exit /b 1)
findstr /c:"phys-p4 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P4 anchor line & exit /b 1)
findstr /c:"phys-p3 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P3 anchor line & exit /b 1)
findstr /c:"phys-p2 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P2 anchor line & exit /b 1)
findstr /c:"phys-p1 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P1 anchor line & exit /b 1)
findstr /c:"phys-p0 selftest: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P0 anchor line & exit /b 1)
type "%D%selftest.log"

echo === check-jobs --threads 4 (adapter matrix: N=1 proof + N=4 determinism) ===
"%TOOLS%" check-jobs --threads 4 --frames 240 > "%D%check-jobs.log" 2>&1
if errorlevel 1 (
    type "%D%check-jobs.log"
    echo [ERROR] check-jobs FAILED
    exit /b 1
)
findstr /c:"drop N=1 adapter vs SingleThreaded: IDENTICAL" "%D%check-jobs.log" >nul || (echo [ERROR] missing drop N=1 hard proof & exit /b 1)
findstr /c:"stack N=1 adapter vs SingleThreaded: IDENTICAL" "%D%check-jobs.log" >nul || (echo [ERROR] missing stack N=1 hard proof & exit /b 1)
findstr /c:"drop N=4 run-to-run: IDENTICAL" "%D%check-jobs.log" >nul || (echo [ERROR] missing drop run-to-run & exit /b 1)
findstr /c:"stack N=4 run-to-run: IDENTICAL" "%D%check-jobs.log" >nul || (echo [ERROR] missing stack run-to-run & exit /b 1)
findstr /c:"phys-j5c check-jobs: PASS" "%D%check-jobs.log" >nul || (echo [ERROR] missing check-jobs PASS & exit /b 1)
type "%D%check-jobs.log"

echo === adapter N=1 full-file byte-compare vs archived M-P0 drop baseline ===
"%TOOLS%" simulate --jobs 1 --frames 240 --out "%D%jobs1-regen.tsv" >nul || exit /b 1
fc /b "%ROOT%\example\phys-p0\drop.tsv" "%D%jobs1-regen.tsv" >nul
if errorlevel 1 (
    echo [ERROR] adapter N=1 file is NOT byte-identical to M-P0 drop.tsv
    exit /b 1
)
echo jobs1-regen.tsv byte-identical to phys-p0\drop.tsv: OK

echo === M-P0..M-P4 baseline byte-identical regression (5 baselines) ===
"%TOOLS%" simulate --out "%D%drop-regen.tsv" >nul || exit /b 1
"%TOOLS%" layers --out "%D%layers-regen.tsv" >nul || exit /b 1
"%TOOLS%" stack --out "%D%stack-regen.tsv" >nul || exit /b 1
"%TOOLS%" debug --out "%D%debug-regen.tsv" >nul || exit /b 1
"%TOOLS%" character --frames 1200 --out "%D%character-regen.tsv" >nul || exit /b 1
python "%ROOT%\tools\check-phys-regression.py" > "%D%regression.log" 2>&1
if errorlevel 1 (
    type "%D%regression.log"
    echo [ERROR] M-P0..M-P4 dumps changed
    exit /b 1
)
type "%D%regression.log"

echo === usage: check-jobs --threads 0 (exit 2 expected) ===
"%TOOLS%" check-jobs --threads 0 > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] bad --threads should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: simulate --jobs -1 (exit 2 expected) ===
"%TOOLS%" simulate --jobs -1 --frames 240 > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --jobs should exit 2
    exit /b 1
)
type "%D%negative.log"

echo === evidence PNG (from real logs) ===
python "%ROOT%\tools\make-phys-j5c-evidence.py" || exit /b 1

echo === PHYS-J5C ACCEPTANCE: ALL GREEN ===
exit /b 0

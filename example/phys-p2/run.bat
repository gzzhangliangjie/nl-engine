@echo off
rem P8 M-P2 acceptance: stacking stability + sleep island + 3-case contrast.
rem All evidence PNGs are regenerated from REAL tool output (stack.tsv + logs).
rem Includes M-P0 + M-P1 byte-identical regression gates.
rem Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\phys-tools.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] phys-tools not built: build\dx12\phys-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest --verbose (M-P0 + M-P1 + M-P2 anchors) ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] phys-tools selftest FAILED
    exit /b 1
)
findstr /c:"phys-p2 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P2 anchor line & exit /b 1)
findstr /c:"phys-p1 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P1 anchor line & exit /b 1)
findstr /c:"phys-p0 selftest: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P0 anchor line & exit /b 1)
type "%D%selftest.log"

echo === stack --out stack.tsv (3 real cases, 6 cubes, 600 frames) ===
"%EXE%" stack --out "%D%stack.tsv" || exit /b 1
findstr /c:"nl-physics M-P2 stacking stability" "%D%stack.tsv" >nul || (echo [ERROR] missing M-P2 TSV header & exit /b 1)
findstr /c:"frame	t	baseline_y0" "%D%stack.tsv" >nul || (echo [ERROR] missing TSV column header & exit /b 1)

echo === check-stack stack.tsv ===
"%EXE%" check-stack "%D%stack.tsv" > "%D%check-stack.log" 2>&1
if errorlevel 1 (
    type "%D%check-stack.log"
    echo [ERROR] stacking dump re-verify FAILED
    exit /b 1
)
type "%D%check-stack.log"

echo === M-P0 + M-P1 baseline byte-identical regression ===
"%EXE%" simulate --out "%D%drop-regen.tsv" >nul || exit /b 1
"%EXE%" layers --out "%D%layers-regen.tsv" >nul || exit /b 1
python "%ROOT%\tools\check-phys-regression.py" > "%D%regression.log" 2>&1
if errorlevel 1 (
    type "%D%regression.log"
    echo [ERROR] M-P0 / M-P1 dumps changed
    exit /b 1
)
type "%D%regression.log"

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: stack --frames 0 (exit 2 expected) ===
"%EXE%" stack --frames 0 > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --frames should exit 2
    exit /b 1
)
type "%D%negative.log"

echo === evidence PNGs (from real TSV + logs) ===
python "%ROOT%\tools\make-phys-p2-evidence.py" || exit /b 1

echo === PHYS-P2 ACCEPTANCE: ALL GREEN ===
exit /b 0

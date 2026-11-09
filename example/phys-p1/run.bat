@echo off
rem P8 M-P1 acceptance: collision layer matrix + engine<->Jolt body association.
rem All evidence PNGs are regenerated from REAL tool output (layers.tsv + logs).
rem Includes an M-P0 byte-identical regression gate (drop.tsv vs phys-p0 baseline).
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

echo === selftest --verbose (M-P0 anchors + M-P1 layer/association/determinism) ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] phys-tools selftest FAILED
    exit /b 1
)
findstr /c:"phys-p1 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P1 anchor line & exit /b 1)
findstr /c:"phys-p0 selftest: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P0 anchor line & exit /b 1)
type "%D%selftest.log"

echo === layers --out layers.tsv (4 real cases) ===
"%EXE%" layers --out "%D%layers.tsv" || exit /b 1
findstr /c:"nl-physics M-P1 collision layer matrix probe" "%D%layers.tsv" >nul || (echo [ERROR] missing M-P1 TSV header & exit /b 1)
findstr /c:"frame	t	same_layer	enabled_pair	disabled_pair	ghost" "%D%layers.tsv" >nul || (echo [ERROR] missing TSV column header & exit /b 1)

echo === check-layers layers.tsv ===
"%EXE%" check-layers "%D%layers.tsv" > "%D%check-layers.log" 2>&1
if errorlevel 1 (
    type "%D%check-layers.log"
    echo [ERROR] layer matrix re-verify FAILED
    exit /b 1
)
type "%D%check-layers.log"

echo === M-P0 baseline byte-identical regression ===
"%EXE%" simulate --out "%D%drop-regen.tsv" >nul || exit /b 1
python "%ROOT%\tools\check-phys-p0-regression.py" > "%D%regression.log" 2>&1
if errorlevel 1 (
    type "%D%regression.log"
    echo [ERROR] M-P0 drop trajectory changed
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

echo === negative: layers --frames 0 (exit 2 expected) ===
"%EXE%" layers --frames 0 > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --frames should exit 2
    exit /b 1
)
type "%D%negative.log"

echo === evidence PNGs (from real TSV + logs) ===
python "%ROOT%\tools\make-phys-p1-evidence.py" || exit /b 1

echo === PHYS-P1 ACCEPTANCE: ALL GREEN ===
exit /b 0

@echo off
rem P8 M-P0 acceptance: headless Jolt 5.3.1 drop world + trajectory dump + checks
rem + evidence PNG regeneration from the real TSV/logs. All output ASCII.
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

echo === selftest --verbose ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] phys-tools selftest FAILED
    exit /b 1
)
type "%D%selftest.log"

echo === simulate --out drop.tsv ===
"%EXE%" simulate --out "%D%drop.tsv" || exit /b 1
findstr /c:"body_count=2" "%D%drop.tsv" >nul || (echo [ERROR] missing body_count header & exit /b 1)
findstr /c:"frame	t	y	vy" "%D%drop.tsv" >nul || (echo [ERROR] missing TSV header & exit /b 1)

echo === check drop.tsv ===
"%EXE%" check "%D%drop.tsv" > "%D%check.log" 2>&1
if errorlevel 1 (
    type "%D%check.log"
    echo [ERROR] trajectory check FAILED
    exit /b 1
)
type "%D%check.log"

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: bad restitution (exit 2 expected) ===
"%EXE%" simulate --restitution 1.5 > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --restitution should exit 2
    exit /b 1
)
type "%D%negative.log"

echo === evidence PNGs (from real TSV + logs) ===
python "%ROOT%\tools\make-phys-evidence.py" || exit /b 1

echo === PHYS-P0 ACCEPTANCE: ALL GREEN ===
exit /b 0

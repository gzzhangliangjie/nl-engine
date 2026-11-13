@echo off
rem WEA M-WEA0 acceptance: weather state layer (A-class track, tool-version
rem deliverable). Gates in one run:
rem   1. selftest weather   - mechanical anchors (determinism / preset / ranges / continuity / negative)
rem   2. dump TSV x2        - auto timeline (seed 42) + preset rain (seed 7), real tool output
rem   3. verify x2          - C++ replay determinism (same seed rebuild + stateless evaluate)
rem   4. verify.py twin     - INDEPENDENT python twin recomputes both TSVs row by row
rem   5. usage/negative     - exit code contract (2 = usage error)
rem   6. evidence PNG       - curves from the real TSV (not hand-drawn)
rem Exit 0 = all green. Build first:
rem   scripts\wt.bat weather build DX12
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"
set "TOOLS=%ROOT%\build\DX12\nanite-tools.exe"

if not exist "%TOOLS%" (
    echo [ERROR] nanite-tools not built: build\DX12\nanite-tools.exe
    echo         build via scripts\wt.bat weather build DX12
    exit /b 1
)

echo === selftest weather (state layer anchors) ===
"%TOOLS%" selftest weather > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] nanite-tools selftest weather FAILED
    exit /b 1
)
findstr /c:"weather selftest: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing selftest PASS line & exit /b 1)
type "%D%selftest.log"

echo === dump auto timeline (seed 42, 1800s, step 2) ===
"%TOOLS%" weather --seed 42 --duration 1800 --step 2 --tsv "%D%weather-auto.tsv" > "%D%dump-auto.log" 2>&1
if errorlevel 1 (
    type "%D%dump-auto.log"
    echo [ERROR] dump auto FAILED
    exit /b 1
)
type "%D%dump-auto.log"

echo === dump preset rain (seed 7, 300s, step 2) ===
"%TOOLS%" weather --seed 7 --duration 300 --step 2 --preset rain --tsv "%D%weather-rain.tsv" > "%D%dump-rain.log" 2>&1
if errorlevel 1 (
    type "%D%dump-rain.log"
    echo [ERROR] dump rain FAILED
    exit /b 1
)
type "%D%dump-rain.log"

echo === C++ replay verify (seed 42 / seed 1234) ===
"%TOOLS%" weather --seed 42 --duration 1800 --verify > "%D%verify-42.log" 2>&1
if errorlevel 1 (type "%D%verify-42.log" & echo [ERROR] verify 42 FAILED & exit /b 1)
type "%D%verify-42.log"
findstr /c:"weather verify: PASS" "%D%verify-42.log" >nul || (echo [ERROR] missing verify PASS line & exit /b 1)
"%TOOLS%" weather --seed 1234 --duration 1200 --verify > "%D%verify-1234.log" 2>&1
if errorlevel 1 (type "%D%verify-1234.log" & echo [ERROR] verify 1234 FAILED & exit /b 1)
type "%D%verify-1234.log"
findstr /c:"weather verify: PASS" "%D%verify-1234.log" >nul || (echo [ERROR] missing verify PASS line & exit /b 1)

echo === independent python twin recomputes both TSVs ===
python "%D%verify.py" "%D%weather-auto.tsv" --seed 42 > "%D%twin-auto.log" 2>&1
if errorlevel 1 (type "%D%twin-auto.log" & echo [ERROR] twin auto FAILED & exit /b 1)
type "%D%twin-auto.log"
python "%D%verify.py" "%D%weather-rain.tsv" --seed 7 --preset rain > "%D%twin-rain.log" 2>&1
if errorlevel 1 (type "%D%twin-rain.log" & echo [ERROR] twin rain FAILED & exit /b 1)
type "%D%twin-rain.log"

echo === usage: no args (exit 2 expected) ===
"%TOOLS%" weather > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: --preset bogus (exit 2 expected) ===
"%TOOLS%" weather --preset bogus > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --preset should exit 2
    exit /b 1
)
type "%D%negative.log"

echo === evidence PNG (from real TSV) ===
python "%ROOT%\tools\make-weather-evidence.py" || exit /b 1

echo === WEA-M0 ACCEPTANCE: ALL GREEN ===
exit /b 0

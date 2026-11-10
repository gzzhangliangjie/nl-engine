@echo off
rem P8 M-GL0 acceptance: GameClock dual-frame model (A-class track, tool-version
rem deliverable). Gates in one run:
rem   1. selftest      - mechanical anchors (accumulate / clamp / pause / scale / determinism)
rem   2. trace const   - 240 frames x 16.7ms -> 60Hz logic cadence, TSV out
rem   3. trace burst   - 5000ms burst + pause/resume + scale mix, TSV out
rem   4. verify.py     - INDEPENDENT python twin recomputes both traces row-by-row
rem   5. usage/negative- exit code contract (2 = usage error)
rem Exit 0 = all green. Build first:
rem   scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"
set "TOOLS=%ROOT%\build\dx12\gl-tools.exe"

if not exist "%TOOLS%" (
    echo [ERROR] gl-tools not built: build\dx12\gl-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest --verbose (GameClock anchors) ===
"%TOOLS%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] gl-tools selftest FAILED
    exit /b 1
)
findstr /c:"gl-m0 selftest: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing selftest PASS line & exit /b 1)
type "%D%selftest.log"

echo === trace constant cadence (240 x 16.7ms) ===
"%TOOLS%" trace --frames 240 --dt 16.7 --out "%D%trace-const.tsv" > "%D%trace-const.log" 2>&1
if errorlevel 1 (
    type "%D%trace-const.log"
    echo [ERROR] trace const FAILED
    exit /b 1
)
findstr /c:"gl-m0 trace: PASS" "%D%trace-const.log" >nul || (echo [ERROR] missing trace PASS line & exit /b 1)
type "%D%trace-const.log"

echo === trace burst + pause/resume + scale mix ===
"%TOOLS%" trace --dts 5000,16.7,16.7,8,16.7,33.4,16.7,0,16.7 --scale 2 --pause-at 4 --resume-at 7 --out "%D%trace-burst.tsv" > "%D%trace-burst.log" 2>&1
if errorlevel 1 (
    type "%D%trace-burst.log"
    echo [ERROR] trace burst FAILED
    exit /b 1
)
findstr /c:"gl-m0 trace: PASS" "%D%trace-burst.log" >nul || (echo [ERROR] missing burst PASS line & exit /b 1)
type "%D%trace-burst.log"

echo === independent verification: python twin recomputes both traces ===
python "%D%verify.py" "%D%trace-const.tsv" --dt 16.7 > "%D%verify-const.log" 2>&1
if errorlevel 1 (
    type "%D%verify-const.log"
    echo [ERROR] verify const FAILED
    exit /b 1
)
type "%D%verify-const.log"
python "%D%verify.py" "%D%trace-burst.tsv" --dts 5000,16.7,16.7,8,16.7,33.4,16.7,0,16.7 --scale 2 --pause-at 4 --resume-at 7 > "%D%verify-burst.log" 2>&1
if errorlevel 1 (
    type "%D%verify-burst.log"
    echo [ERROR] verify burst FAILED
    exit /b 1
)
type "%D%verify-burst.log"

echo === usage: no args (exit 2 expected) ===
"%TOOLS%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: trace --dt abc (exit 2 expected) ===
"%TOOLS%" trace --dt abc > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --dt should exit 2
    exit /b 1
)
type "%D%negative.log"

echo === evidence PNG (from real logs) ===
python "%ROOT%\tools\make-gl-evidence.py" || exit /b 1

echo === GL-M0 ACCEPTANCE: ALL GREEN ===
exit /b 0

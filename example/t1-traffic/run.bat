@echo off
rem T0 M-T1.0~T1.2 acceptance: signal phases (green/yellow/clear) + reservation
rem locks + car-following under intersection control. All output ASCII.
rem Exit 0 = all green. Build first: scripts\wt.bat worldgen build DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\traffic-tools.exe"
set "D=%~dp0"
set "CITY=%ROOT%\example\w0-m0\roadnet.json"

if not exist "%EXE%" (
    echo [ERROR] traffic-tools not built: build\dx12\traffic-tools.exe
    echo         build via scripts\wt.bat worldgen build DX12
    exit /b 1
)
if not exist "%CITY%" (
    echo [ERROR] city roadnet missing: example\w0-m0\roadnet.json
    exit /b 1
)

echo === selftest --verbose (scheduler anchors included) ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] traffic selftest FAILED
    exit /b 1
)
type "%D%selftest.log"

echo === sim signalized fixture 6 cars (all 4 nodes signal, default 8/2/1) ===
"%EXE%" sim "%D%signal-roadnet.json" --cars 6 --frames 1800 --png "%D%signal-sim.png" > "%D%sim-signal.log" 2>&1
if errorlevel 1 (
    type "%D%sim-signal.log"
    echo [ERROR] signal sim FAILED
    exit /b 1
)
type "%D%sim-signal.log"
findstr /C:"signals     : 4 (lock-only 0 / signal 4)" "%D%sim-signal.log" >nul
if errorlevel 1 ( echo [ERROR] expected 4 signal nodes & exit /b 1 )
findstr /C:"locked end  : 0" "%D%sim-signal.log" >nul
if errorlevel 1 ( echo [ERROR] expected zero locks held at end & exit /b 1 )

echo === sim real city 32 cars (49 lock-only nodes) + final-frame png ===
"%EXE%" sim "%CITY%" --cars 32 --frames 1800 --png "%D%city-sim.png" > "%D%sim-city.log" 2>&1
if errorlevel 1 (
    type "%D%sim-city.log"
    echo [ERROR] city sim FAILED
    exit /b 1
)
type "%D%sim-city.log"
findstr /C:"locked end  : 0" "%D%sim-city.log" >nul
if errorlevel 1 ( echo [ERROR] deadlock detected & exit /b 1 )
findstr /C:"min gap     : " "%D%sim-city.log" >nul
if errorlevel 1 ( echo [ERROR] min gap line missing & exit /b 1 )

echo === negative: cross-road collision guard is active (selftest covers it) ===
"%EXE%" path "%D%signal-roadnet.json" --from 0 --to 99 > "%D%negative.log" 2>&1
if errorlevel 1 (
    echo negative exit 1 OK
) else (
    echo [ERROR] unknown edge should exit 1
    exit /b 1
)
type "%D%negative.log"

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo.
echo t1-traffic run.bat ALL GREEN
endlocal

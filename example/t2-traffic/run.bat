@echo off
rem T0 M-T2.0~T2.2 acceptance: group-shared locks + turn-exclusion, gap-out
rem actuated signals, loop routes. All output ASCII. Exit 0 = all green.
rem Build first: scripts\wt.bat worldgen build DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\traffic-tools.exe"
set "D=%~dp0"
set "CITY=%ROOT%\example\w0-m0\roadnet.json"
set "FLAT=%ROOT%\example\t0-traffic\roadnet.json"

if not exist "%EXE%" (
    echo [ERROR] traffic-tools not built: build\dx12\traffic-tools.exe
    echo         build via scripts\wt.bat worldgen build DX12
    exit /b 1
)
if not exist "%CITY%" (
    echo [ERROR] city roadnet missing: example\w0-m0\roadnet.json
    exit /b 1
)

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === selftest --verbose (4685 asserts, section 10 M-T2 anchors) ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] traffic selftest FAILED
    exit /b 1
)
type "%D%selftest.log"

echo === sim gap-out fixture 6 cars (4 signal nodes, gap_out_s 1.0 via phases) ===
"%EXE%" sim "%D%gapout-roadnet.json" --cars 6 --frames 1800 --png "%D%gapout-sim.png" > "%D%sim-gapout.log" 2>&1
if errorlevel 1 (
    type "%D%sim-gapout.log"
    echo [ERROR] gap-out sim FAILED
    exit /b 1
)
type "%D%sim-gapout.log"
findstr /C:"signals     : 4 (lock-only 0 / signal 4)" "%D%sim-gapout.log" >nul
if errorlevel 1 ( echo [ERROR] expected 4 signal nodes & exit /b 1 )
findstr /C:"locked end  : 0" "%D%sim-gapout.log" >nul
if errorlevel 1 ( echo [ERROR] expected zero locks held at end & exit /b 1 )

echo === sim real city 32 cars (group-shared locks throughput) ===
"%EXE%" sim "%CITY%" --cars 32 --frames 1800 --png "%D%city-sim.png" > "%D%sim-city.log" 2>&1
if errorlevel 1 (
    type "%D%sim-city.log"
    echo [ERROR] city sim FAILED
    exit /b 1
)
type "%D%sim-city.log"
findstr /C:"arrived     : 17 / 32" "%D%sim-city.log" >nul
if errorlevel 1 ( echo [ERROR] expected arrived 17/32 & exit /b 1 )
findstr /C:"distance    : 646.9 m" "%D%sim-city.log" >nul
if errorlevel 1 ( echo [ERROR] expected distance 646.9 m & exit /b 1 )

echo === sim loop routes 4 cars (closed rings, never parks) ===
"%EXE%" sim "%FLAT%" --cars 4 --frames 1800 --loop > "%D%sim-loop.log" 2>&1
if errorlevel 1 (
    type "%D%sim-loop.log"
    echo [ERROR] loop sim FAILED
    exit /b 1
)
type "%D%sim-loop.log"
findstr /C:"loop routes : 4 / 4 (closed rings)" "%D%sim-loop.log" >nul
if errorlevel 1 ( echo [ERROR] expected 4 closed rings & exit /b 1 )

echo === negative: unknown edge exits 1 ===
"%EXE%" path "%D%gapout-roadnet.json" --from 0 --to 99 > "%D%negative.log" 2>&1
if errorlevel 1 (
    echo negative exit 1 OK
) else (
    echo [ERROR] unknown edge should exit 1
    exit /b 1
)
type "%D%negative.log"

echo.
echo t2-traffic run.bat ALL GREEN
endlocal

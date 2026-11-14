@echo off
rem T0 M-T0.0~T0.2 acceptance: traffic lane graph + A* path + car-following sim.
rem All output ASCII. Exit 0 = all green. Build first: scripts\wt.bat worldgen build DX12
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

echo === selftest --verbose ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] traffic selftest FAILED
    exit /b 1
)
type "%D%selftest.log"

echo === build fixture lane graph + deterministic dump ===
"%EXE%" build "%D%roadnet.json" --dump "%D%t0-lane-graph.json" --stats > "%D%build.log" 2>&1
if errorlevel 1 (
    type "%D%build.log"
    echo [ERROR] build FAILED
    exit /b 1
)
type "%D%build.log"

echo === path fixture e0 to e3 (unique 4 hops) ===
"%EXE%" path "%D%roadnet.json" --from 0 --to 3 > "%D%path.log" 2>&1
if errorlevel 1 (
    type "%D%path.log"
    echo [ERROR] path FAILED
    exit /b 1
)
type "%D%path.log"

echo === sim real city 32 cars (--no-signals pins lock-only baseline) ===
"%EXE%" sim "%CITY%" --no-signals --cars 32 --frames 1800 --png "%D%city-sim.png" > "%D%sim.log" 2>&1
if errorlevel 1 (
    type "%D%sim.log"
    echo [ERROR] sim FAILED
    exit /b 1
)
type "%D%sim.log"
findstr /C:"signals     : 49 (lock-only 49 / signal 0)" "%D%sim.log" >nul
if errorlevel 1 ( echo [ERROR] expected lock-only 49 & exit /b 1 )

echo === lanes real city png ===
"%EXE%" lanes "%CITY%" --png "%D%city-lanes.png" > "%D%lanes.log" 2>&1
if errorlevel 1 (
    type "%D%lanes.log"
    echo [ERROR] lanes FAILED
    exit /b 1
)
type "%D%lanes.log"

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: path to unknown edge 99 (exit 1 expected) ===
"%EXE%" path "%D%roadnet.json" --from 0 --to 99 > "%D%negative.log" 2>&1
if errorlevel 1 (
    echo negative exit 1 OK
) else (
    echo [ERROR] unknown edge should exit 1
    exit /b 1
)
type "%D%negative.log"

echo.
echo t0-traffic run.bat ALL GREEN
endlocal

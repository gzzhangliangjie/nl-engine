@echo off
rem M-PQ1 acceptance: live DebugScene query via QueryScene attach + dynamic
rem body registration. Structural anchors only (no numeric TSV). All ASCII.
rem Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12
rem Usage: run.bat [live]  |  no extra args = live (default)
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\phys-tools.exe"
set "D=%~dp0"

if "%~1"=="" goto live
if /i "%~1"=="live" goto live
echo [ERROR] unknown argument: %~1
echo usage: run.bat [live]
exit /b 2

:live
if not exist "%EXE%" (
    echo [ERROR] phys-tools not built: build\dx12\phys-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === query-live --frames 90 ===
"%EXE%" query-live --frames 90 > "%D%querylive.log" 2>&1
if errorlevel 1 (
    type "%D%querylive.log"
    echo [ERROR] query-live FAILED
    exit /b 1
)
type "%D%querylive.log"

echo === selftest --verbose covers M-PQ1 live section ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] selftest FAILED
    exit /b 1
)
findstr /c:"phys-pq1 query-live: PASS" "%D%selftest.log" >nul || (
    echo [ERROR] selftest missing M-PQ1 live PASS line
    exit /b 1
)

echo === usage exit 2 expected ===
"%EXE%" query-live --bogus > "%D%usage.log" 2>&1
if not errorlevel 1 (
    echo [ERROR] bad flag should exit nonzero
    exit /b 1
)

echo M-PQ1 showcase: ALL GREEN
exit /b 0

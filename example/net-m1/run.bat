@echo off
rem A-class NET M-NET1 acceptance: vendored bitsery 5.2.5 + vehicle transform
rem snapshot codec (243 bits -> 31 bytes). ASCII only. Exit 0 = all green.
rem Build first: scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\net-tools.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] net-tools not built: build\dx12\net-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === version anchor (vendored traceability: enet + bitsery) ===
"%EXE%" version <nul > "%D%version.log" 2>&1
if errorlevel 1 (echo [ERROR] version FAILED & exit /b 1)
findstr /c:"enet        : 1.3.18" "%D%version.log" <nul >nul || (echo [ERROR] enet anchor missing & exit /b 1)
findstr /c:"bitsery     : 5.2.5" "%D%version.log" <nul >nul || (echo [ERROR] bitsery anchor missing & exit /b 1)
type "%D%version.log"

echo === usage (expect exit 2 = usage error, nav-tools convention) ===
"%EXE%" <nul > "%D%usage.log" 2>&1
if not errorlevel 1 (echo [ERROR] usage should exit 2 & exit /b 1)
findstr /c:"net-tools - nl-engine transport acceptance tool" "%D%usage.log" <nul >nul || (echo [ERROR] usage header missing & exit /b 1)
type "%D%usage.log"

echo === snapshot-selftest (roundtrip bytes-level + quant precision asserts) ===
"%EXE%" snapshot-selftest <nul > "%D%snapshot-selftest.log" 2>&1
if errorlevel 1 (type "%D%snapshot-selftest.log" & echo [ERROR] snapshot-selftest FAILED & exit /b 1)
findstr /c:"snapshot selftest: PASS" "%D%snapshot-selftest.log" <nul >nul || (echo [ERROR] PASS anchor missing & exit /b 1)
type "%D%snapshot-selftest.log"

echo === snapshot-bench (deterministic payload benchmark, seed 1234) ===
"%EXE%" snapshot-bench --count 20000 <nul > "%D%snapshot-bench.log" 2>&1
if errorlevel 1 (type "%D%snapshot-bench.log" & echo [ERROR] snapshot-bench FAILED & exit /b 1)
findstr /c:"snapshot-bench : OK" "%D%snapshot-bench.log" <nul >nul || (echo [ERROR] OK anchor missing & exit /b 1)
findstr /c:"avgBytes=31.00" "%D%snapshot-bench.log" <nul >nul || (echo [ERROR] 31-byte wire anchor missing & exit /b 1)
type "%D%snapshot-bench.log"

echo === negative: truncated stream must fail explicitly (exit 1) ===
"%EXE%" snapshot-decode 01020304 <nul > "%D%negative.log" 2>&1
if not errorlevel 1 (echo [ERROR] truncated stream should exit 1 & exit /b 1)
findstr /c:"decode       : FAILED" "%D%negative.log" <nul >nul || (echo [ERROR] explicit reason missing & exit /b 1)
type "%D%negative.log"

echo.
echo ALL GREEN: net-m1 acceptance
exit /b 0

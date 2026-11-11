@echo off
rem A-class M-LOG0 acceptance: nl::core::log spdlog core swap (vendored v1.17.0)
rem stdout anchors + file sink (Q-L2) + filter instruction demo (M-LOG1) + negatives.
rem ASCII only. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\log-tools.exe"
set "D=%~dp0"
set "RUNDIR=%CD%"

if not exist "%EXE%" (
    echo [ERROR] log-tools not built: build\dx12\log-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === usage (expect exit 2) ===
"%EXE%" <nul > "%D%usage.log" 2>&1
if not errorlevel 1 (echo [ERROR] usage should exit 2 & exit /b 1)
findstr /c:"log-tools - nl-engine logging tools" "%D%usage.log" <nul >nul || (echo [ERROR] usage header missing & exit /b 1)
type "%D%usage.log"

echo === selftest (M0.1 compat + file sink + filters + concurrency) ===
"%EXE%" selftest <nul > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] selftest FAILED
    exit /b 1
)
findstr /c:"log selftest: PASS" "%D%selftest.log" <nul >nul || (echo [ERROR] PASS anchor missing & exit /b 1)
findstr /c:"[nl:info] hello 42" "%D%selftest.log" <nul >nul || (echo [ERROR] M0.1 anchor missing & exit /b 1)
type "%D%selftest.log"

echo === demo: filter nl:info,renderer=warn (before vs after) ===
"%EXE%" demo --filter "nl:info,renderer=warn" <nul > "%D%demo.log" 2>&1
if errorlevel 1 (echo [ERROR] demo FAILED & exit /b 1)
findstr /c:"[nl:debug] [before] plain debug line" "%D%demo.log" <nul >nul || (echo [ERROR] before debug line missing & exit /b 1)
findstr /c:"[nl:info] [after] plain info line" "%D%demo.log" <nul >nul || (echo [ERROR] after info missing & exit /b 1)
findstr /c:"[nl:debug] [after]" "%D%demo.log" <nul >nul && (echo [ERROR] after debug should be muted & exit /b 1)
findstr /c:"[nl:info|renderer] [after]" "%D%demo.log" <nul >nul && (echo [ERROR] after renderer info should be muted & exit /b 1)
findstr /c:"[nl:warn|renderer] [after] renderer warn" "%D%demo.log" <nul >nul || (echo [ERROR] after renderer warn missing & exit /b 1)
findstr /c:"[nl:info|asset] [after] asset info" "%D%demo.log" <nul >nul || (echo [ERROR] after asset info missing & exit /b 1)
findstr /c:"filter instruction (applied): nl:info,renderer=warn" "%D%demo.log" <nul >nul || (echo [ERROR] instruction roundtrip missing & exit /b 1)
type "%D%demo.log"

echo === negative: invalid filter must fail explicitly (exit 1) ===
"%EXE%" demo --filter bogus <nul > "%D%negative.log" 2>&1
if not errorlevel 1 (echo [ERROR] invalid filter should exit 1 & exit /b 1)
findstr /c:"invalid filter segment 'bogus'" "%D%negative.log" <nul >nul || (echo [ERROR] explicit reason missing & exit /b 1)
type "%D%negative.log"

echo === file sink Q-L2: newest log file carries stdout anchors ===
set "LOGDIR=%RUNDIR%\out\log"
if not exist "%LOGDIR%" (echo [ERROR] out\log not created & exit /b 1)
for /f "delims=" %%F in ('dir /b /o-d "%LOGDIR%\log-tools-DX12-*.log" 2^>nul') do (
    findstr /c:"[nl:info] hello 42" "%LOGDIR%\%%F" <nul >nul && (
        echo file sink OK: out\log\%%F
        goto fileok
    )
)
echo [ERROR] no log file carrying [nl:info] anchor
exit /b 1
:fileok

echo.
echo ALL GREEN: log-m0 acceptance
exit /b 0

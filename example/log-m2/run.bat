@echo off
rem A-class M-LOG2 acceptance: frame ctx + console log.* + file rolling + channel migration
rem stdout anchors + exec chain (console single-source) + negatives + file evidence.
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

echo === usage (expect exit 2, exec listed) ===
"%EXE%" <nul > "%D%usage.log" 2>&1
if not errorlevel 1 (echo [ERROR] usage should exit 2 & exit /b 1)
findstr /c:"log-tools exec" "%D%usage.log" <nul >nul || (echo [ERROR] exec line missing & exit /b 1)
type "%D%usage.log"

echo === exec chain: log.status / log.filter / log.channel (single-source commands) ===
"%EXE%" exec "log.status" "log.filter nl:info,renderer=warn" "log.channel asset off" "log.status" <nul > "%D%exec.log" 2>&1
if errorlevel 1 (echo [ERROR] exec chain FAILED & exit /b 1)
findstr /c:"log filter: nl:debug" "%D%exec.log" <nul >nul || (echo [ERROR] initial status missing & exit /b 1)
findstr /c:"log filter -> nl:info,renderer=warn" "%D%exec.log" <nul >nul || (echo [ERROR] filter apply missing & exit /b 1)
findstr /c:"log filter -> nl:info,renderer=warn,asset=off" "%D%exec.log" <nul >nul || (echo [ERROR] channel off missing & exit /b 1)
findstr /c:"log filter: nl:info,renderer=warn,asset=off" "%D%exec.log" <nul >nul || (echo [ERROR] status roundtrip missing & exit /b 1)
type "%D%exec.log"

echo === exec negative: invalid level explicit rejection (exit 1) ===
"%EXE%" exec "log.level bogus" <nul > "%D%negative.log" 2>&1
if not errorlevel 1 (echo [ERROR] invalid level should exit 1 & exit /b 1)
findstr /c:"log.level: invalid filter segment 'bogus'" "%D%negative.log" <nul >nul || (echo [ERROR] explicit reason missing & exit /b 1)
type "%D%negative.log"

echo === selftest (8 segments incl. frame ctx + file rolling) ===
"%EXE%" selftest <nul > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] selftest FAILED
    exit /b 1
)
findstr /c:"log selftest: PASS" "%D%selftest.log" <nul >nul || (echo [ERROR] PASS anchor missing & exit /b 1)
findstr /c:"segment 7: M-LOG2 frame context injection" "%D%selftest.log" <nul >nul || (echo [ERROR] frame segment missing & exit /b 1)
findstr /c:"segment 8: M-LOG2 file rolling" "%D%selftest.log" <nul >nul || (echo [ERROR] rolling segment missing & exit /b 1)
type "%D%selftest.log"

echo === frame suffix evidence: viewer render log carries "#<frame>" tail ===
set "LOGDIR=%RUNDIR%\..\..\build\dx12\out\log"
set "FOUND="
for /f "delims=" %%F in ('dir /b /o-d "%LOGDIR%\viewer-DX12-*.log" 2^>nul') do (
    if not defined FOUND (
        findstr /r /c:"\[nl:info|viewer\].*#[0-9][0-9]*$" "%LOGDIR%\%%F" <nul >nul && (
            echo frame suffix OK: viewer-DX12 log %%F carries "#<frame>" tails
            set "FOUND=%%F"
        )
    )
)
if not defined FOUND (
    echo [ERROR] no viewer log with frame-suffixed viewer-channel line
    echo         run: viewer.exe --backend dx12 --frames 3 first
    exit /b 1
)

echo.
echo ALL GREEN: log-m2 acceptance
exit /b 0

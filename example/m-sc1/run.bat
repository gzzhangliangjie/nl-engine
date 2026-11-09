@echo off
rem M-SC1 (C# script track) acceptance: console binding selftest (s6a-s6f)
rem + run --api demo + negative getEntryPoint, then evidence PNG regeneration
rem from real output. Exit 0 = all green.
rem Requires: build\DX12\nl-script.exe (scripts\build-m0.2.bat DX12) and dotnet SDK on PATH.
setlocal enabledelayedexpansion
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\nl-script.exe"
set "LOGS=%~dp0"
set "HELLO=%ROOT%\script\examples\hello"
set "CONS=%ROOT%\script\examples\console"
set "TYPE=NlScript.Examples.ConsoleScript, ConsoleScript"

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\DX12\nl-script.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest full (hosting s0-s5 + console binding s6a-s6f) ===
"%EXE%" selftest "%HELLO%" "%CONS%" > "%LOGS%selftest.log" || exit /b 1
type "%LOGS%selftest.log"

echo === run --api demo (positive, exit 0 expected) ===
"%EXE%" run "%CONS%" --api --type "%TYPE%" > "%LOGS%run-api.log" || exit /b 1
type "%LOGS%run-api.log"

echo === run --api missing entry type (negative: getEntryPoint Result error, exit 1) ===
"%EXE%" run "%CONS%" --api > "%LOGS%run-badtype.log" 2>&1
if not errorlevel 1 (
    echo [ERROR] bad-type case should exit 1
    exit /b 1
)
if errorlevel 2 (
    echo [ERROR] bad-type case should exit 1
    exit /b 1
)
type "%LOGS%run-badtype.log"

echo === check console example (compile only, exit 0 expected) ===
"%EXE%" check "%CONS%" > "%LOGS%check.log" || exit /b 1
type "%LOGS%check.log"

echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-script-evidence.py" m-sc1 || exit /b 1

echo === M-SC1 ACCEPTANCE: ALL GREEN ===
exit /b 0

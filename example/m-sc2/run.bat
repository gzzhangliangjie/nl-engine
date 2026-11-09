@echo off
rem M-SC2 (C# script track) acceptance: ECS data-plane binding selftest (s7a-s7g,
rem on top of hosting s0-s5 + console s6a-s6f) + run --api ecs demo + negative
rem wrong-type getEntryPoint, then evidence PNG regeneration from real output.
rem Exit 0 = all green.
rem Requires: build\DX12\nl-script.exe (scripts\build-m0.2.bat DX12) and dotnet SDK on PATH.
setlocal enabledelayedexpansion
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\nl-script.exe"
set "LOGS=%~dp0"
set "HELLO=%ROOT%\script\examples\hello"
set "CONS=%ROOT%\script\examples\console"
set "ECSP=%ROOT%\script\examples\ecs"

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\DX12\nl-script.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest full (hosting s0-s5 + console s6a-s6f + ECS s7a-s7g) ===
"%EXE%" selftest "%HELLO%" "%CONS%" "%ECSP%" > "%LOGS%selftest.log" || exit /b 1
type "%LOGS%selftest.log"

echo === run --api ecs demo (positive, exit 0 expected) ===
"%EXE%" run "%ECSP%" --api > "%LOGS%run-api.log" || exit /b 1
type "%LOGS%run-api.log"

echo === run --api wrong --type (negative: getEntryPoint Result error, exit 1) ===
"%EXE%" run "%ECSP%" --api --type "NlScript.Examples.HelloScript, EcsScript" > "%LOGS%run-badtype.log" 2>&1
if not errorlevel 1 (
    echo [ERROR] bad-type case should exit 1
    exit /b 1
)
if errorlevel 2 (
    echo [ERROR] bad-type case should exit 1
    exit /b 1
)
type "%LOGS%run-badtype.log"

echo === check ecs example (compile only, exit 0 expected) ===
"%EXE%" check "%ECSP%" > "%LOGS%check.log" || exit /b 1
type "%LOGS%check.log"

echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-script-evidence.py" m-sc2 || exit /b 1

echo === M-SC2 ACCEPTANCE: ALL GREEN ===
exit /b 0

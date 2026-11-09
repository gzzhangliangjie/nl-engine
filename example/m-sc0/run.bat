@echo off
rem M-SC0 (C# script track) acceptance: selftest + run positive/negative + check
rem + usage error, then evidence PNG regeneration from real output. Exit 0 = all green.
rem Requires: build\dx12\nl-script.exe (scripts\build-m0.2.bat DX12) and dotnet SDK on PATH.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\nl-script.exe"
set "LOGS=%~dp0"
set "PROJ=%ROOT%\script\examples\hello"

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\DX12\nl-script.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest (full machine-checked acceptance) ===
"%EXE%" selftest "%PROJ%" > "%LOGS%selftest.log" || exit /b 1
type "%LOGS%selftest.log"

echo === run --args ok (positive, exit 0 expected) ===
"%EXE%" run "%PROJ%" --args ok > "%LOGS%run-ok.log" || exit /b 1
type "%LOGS%run-ok.log"

echo === run --args throw (negative: managed exception, exit 1 expected) ===
"%EXE%" run "%PROJ%" --args throw > "%LOGS%run-throw.log" 2>&1
if errorlevel 2 (
    echo [ERROR] throw case should exit 1
    exit /b 1
)
if not errorlevel 1 (
    echo [ERROR] throw case should exit 1
    exit /b 1
)
type "%LOGS%run-throw.log"

echo === check (compile only, exit 0 expected) ===
"%EXE%" check "%PROJ%" > "%LOGS%check.log" || exit /b 1
type "%LOGS%check.log"

echo === usage (no args, exit 2 expected) ===
"%EXE%" > "%LOGS%usage.log" 2>&1
if not errorlevel 2 (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%LOGS%usage.log"

echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-script-evidence.py" || exit /b 1

echo === M-SC0 ACCEPTANCE: ALL GREEN ===
exit /b 0

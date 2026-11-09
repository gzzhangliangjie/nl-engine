@echo off
rem M-E3 (ECS E1a) acceptance: four selftest suites + negative deserialise-check
rem + evidence PNG regeneration from real output. All output ASCII. Exit 0 = all green.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\nanite-scene.exe"
set "LOGS=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] nanite-scene not built: build\dx12\nanite-scene.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest ecs-core (M-E0 regression) ===
"%EXE%" selftest --suite ecs-core || exit /b 1
echo === selftest ecs-hier (M-E1 regression) ===
"%EXE%" selftest --suite ecs-hier || exit /b 1
echo === selftest ecs-system (M-E2, verbose evidence log) ===
"%EXE%" selftest --suite ecs-system --verbose > "%LOGS%system.log" || exit /b 1
type "%LOGS%system.log"
echo === selftest ecs-serial (M-E2, verbose evidence log) ===
"%EXE%" selftest --suite ecs-serial --verbose > "%LOGS%serial.log" || exit /b 1
type "%LOGS%serial.log"
echo === usage log (exit 2 expected) ===
"%EXE%" > "%LOGS%usage.log" 2>&1
if not errorlevel 2 (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%LOGS%usage.log"
echo === negative: deserialise-check badjson (exit 2 expected) ===
"%EXE%" deserialise-check badjson > "%LOGS%negative.log" 2>&1
if not errorlevel 2 (
    echo [ERROR] badjson should exit 2
    exit /b 1
)
type "%LOGS%negative.log"
echo === deserialise-check good (exit 0 expected) ===
"%EXE%" deserialise-check good || exit /b 1
echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-ecs-evidence.py" || exit /b 1

echo === ECS-E1A ACCEPTANCE: ALL GREEN ===
exit /b 0

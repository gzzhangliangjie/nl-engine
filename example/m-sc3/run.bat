@echo off
rem M-SC3 (C# script track) acceptance: hot reload via collectible ALC -- full selftest
rem (hosting s0-s5 + console s6a-s6f + ECS s7a-s7g + hot-reload s8a-s8h) + run --api demo
rem + bootstrap compile check, then evidence PNG regeneration from real output.
rem Exit 0 = all green.
rem Requires: build\DX12\nl-script.exe (scripts\build-m0.2.bat DX12) and dotnet SDK on PATH.
setlocal enabledelayedexpansion
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\nl-script.exe"
set "LOGS=%~dp0"
set "HELLO=%ROOT%\script\examples\hello"
set "CONS=%ROOT%\script\examples\console"
set "ECSP=%ROOT%\script\examples\ecs"
set "BOOT=%ROOT%\script\bootstrap"

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\DX12\nl-script.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest full (s0-s5 hosting + s6 console + s7 ECS + s8 hot reload) ===
"%EXE%" selftest "%HELLO%" "%CONS%" "%ECSP%" "%BOOT%" > "%LOGS%selftest.log" || exit /b 1
type "%LOGS%selftest.log"

echo === s8 hot-reload section close-up (filtered from selftest.log) ===
python -c "import io,sys; ls=io.open(r'%LOGS%selftest.log',encoding='utf-8',errors='replace').read().splitlines(); keep=[l for l in ls if ('s8' in l or 'reload-script:' in l or l.startswith('nl-script selftest') or l.startswith('$ set DOTNET') or 'Build succeeded' in l or l.startswith('  handle'))]; io.open(r'%LOGS%reload.log','w',encoding='utf-8',newline='\n').write('\n'.join(keep)+'\n')" || exit /b 1
type "%LOGS%reload.log"

echo === run --api console demo (positive, exit 0 expected) ===
"%EXE%" run "%CONS%" --api > "%LOGS%run-api.log" || exit /b 1
type "%LOGS%run-api.log"

echo === check bootstrap example (compile only, exit 0 expected) ===
"%EXE%" check "%BOOT%" > "%LOGS%check.log" || exit /b 1
type "%LOGS%check.log"

echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-script-evidence.py" m-sc3 || exit /b 1

echo === M-SC3 ACCEPTANCE: ALL GREEN ===
exit /b 0

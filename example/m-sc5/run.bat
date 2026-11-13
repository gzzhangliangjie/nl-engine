@echo off
rem M-SC5 (C# script track) acceptance: Time/Input/Log domain exports -- full selftest
rem (s0-s5 + s6 + s7 + s8 + s9a/s9b) incl. unbound-window fallbacks, s9 close-up filter,
rem log channel anchor filter, timeinput example compile check, then evidence PNG
rem regeneration from real output. Exit 0 = all green.
rem Requires: build\DX12\nl-script.exe and dotnet SDK on PATH.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\nl-script.exe"
set "LOGS=%~dp0"
set "HELLO=%ROOT%\script\examples\hello"
set "CONS=%ROOT%\script\examples\console"
set "ECSP=%ROOT%\script\examples\ecs"
set "BOOT=%ROOT%\script\bootstrap"
set "TI=%ROOT%\script\examples\timeinput"

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\DX12\nl-script.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest full (s0-s5 + s6 console + s7 ECS + s8 hot reload + s9 Time/Input/Log) ===
"%EXE%" selftest "%HELLO%" "%CONS%" "%ECSP%" "%BOOT%" "%TI%" > "%LOGS%selftest.log" || exit /b 1
type "%LOGS%selftest.log"

echo === s9 section close-up (filtered from selftest.log) ===
python -c "import io,sys; ls=io.open(r'%LOGS%selftest.log',encoding='utf-8',errors='replace').read().splitlines(); keep=[l for l in ls if ('s9' in l or l.startswith('nl-script selftest'))]; io.open(r'%LOGS%s9.log','w',encoding='utf-8',newline='\n').write('\n'.join(keep)+'\n')" || exit /b 1
type "%LOGS%s9.log"

echo === log channel anchor close-up: [nl:info^|script] lines (filtered) ===
type nul > "%LOGS%log-channel.log"
findstr /L /C:"[nl:info" "%LOGS%selftest.log" | findstr /L /C:"script]" >> "%LOGS%log-channel.log" || exit /b 1
findstr /L /C:"s9a3" "%LOGS%selftest.log" >> "%LOGS%log-channel.log" || exit /b 1
type "%LOGS%log-channel.log"

echo === check timeinput example (compile only, exit 0 expected) ===
"%EXE%" check "%TI%" > "%LOGS%check.log" || exit /b 1
type "%LOGS%check.log"

echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-script-evidence.py" m-sc5 || exit /b 1

echo === M-SC5 ACCEPTANCE: ALL GREEN ===
exit /b 0

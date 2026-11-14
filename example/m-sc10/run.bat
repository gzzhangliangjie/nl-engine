@echo off
rem M-SC10 (C# script track) acceptance: script property panel (v7, ApiTable 37->39)
rem -- gen.py --check golden byte-compare (39 exports incl. props 2), full selftest
rem (s0-s5 + s6 + s7 + s8 + s9 + s10 + s11 + s12 + s13a-s13e props probes),
rem s13 close-up filter, props example compile check, then evidence PNG regeneration
rem from real output. Exit 0 = all green.
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
set "CAM=%ROOT%\script\examples\camera"
set "MODEL=%ROOT%\script\examples\model"
set "DRIVE=%ROOT%\script\examples\drive"
set "PROPS=%ROOT%\script\examples\props"

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\DX12\nl-script.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === gen.py --check (golden byte-compare, 39 exports, exit 0 expected) ===
python "%ROOT%\tools\scriptapi\gen.py" --check > "%LOGS%gen-check.log" || exit /b 1
type "%LOGS%gen-check.log"

echo === selftest full (s0-s12 + s13a-s13e props panel) ===
"%EXE%" selftest "%HELLO%" "%CONS%" "%ECSP%" "%BOOT%" "%TI%" "%CAM%" "%MODEL%" "%DRIVE%" "%PROPS%" > "%LOGS%selftest.log" || exit /b 1
type "%LOGS%selftest.log"

echo === s13 section close-up (filtered from selftest.log) ===
python -c "import io,sys; ls=io.open(r'%LOGS%selftest.log',encoding='utf-8',errors='replace').read().splitlines(); keep=[l for l in ls if ('s13' in l or l.startswith('nl-script selftest'))]; io.open(r'%LOGS%s13.log','w',encoding='utf-8',newline='\n').write('\n'.join(keep)+'\n')" || exit /b 1
type "%LOGS%s13.log"

echo === check props example (compile only, exit 0 expected) ===
"%EXE%" check "%PROPS%" > "%LOGS%check.log" || exit /b 1
type "%LOGS%check.log"

echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-script-evidence.py" m-sc10 || exit /b 1

echo === M-SC10 ACCEPTANCE: ALL GREEN ===
exit /b 0

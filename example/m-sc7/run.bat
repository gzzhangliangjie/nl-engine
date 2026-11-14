@echo off
rem M-SC7 (C# script track) acceptance: ECS model attach (v5, ApiTable 29->32)
rem -- gen.py --check golden byte-compare (32 exports incl. ecs model 3 + Init-offset
rem bugfix), full selftest (s0-s5 + s6 + s7 + s8 + s9 + s10 + s11a-s11d model probes),
rem s11 close-up filter, model example compile check, then evidence PNG regeneration
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

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\DX12\nl-script.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === gen.py --check (golden byte-compare, 32 exports, exit 0 expected) ===
python "%ROOT%\tools\scriptapi\gen.py" --check > "%LOGS%gen-check.log" || exit /b 1
type "%LOGS%gen-check.log"

echo === selftest full (s0-s5 + s6 console + s7 ECS + s8 hot reload + s9 Time/Input/Log + s10 Camera + s11 Model) ===
"%EXE%" selftest "%HELLO%" "%CONS%" "%ECSP%" "%BOOT%" "%TI%" "%CAM%" "%MODEL%" > "%LOGS%selftest.log" || exit /b 1
type "%LOGS%selftest.log"

echo === s11 section close-up (filtered from selftest.log) ===
python -c "import io,sys; ls=io.open(r'%LOGS%selftest.log',encoding='utf-8',errors='replace').read().splitlines(); keep=[l for l in ls if ('s11' in l or l.startswith('nl-script selftest'))]; io.open(r'%LOGS%s11.log','w',encoding='utf-8',newline='\n').write('\n'.join(keep)+'\n')" || exit /b 1
type "%LOGS%s11.log"

echo === check model example (compile only, exit 0 expected) ===
"%EXE%" check "%MODEL%" > "%LOGS%check.log" || exit /b 1
type "%LOGS%check.log"

echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-script-evidence.py" m-sc7 || exit /b 1

echo === M-SC7 ACCEPTANCE: ALL GREEN ===
exit /b 0

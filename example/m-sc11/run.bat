@echo off
rem M-SC11 (C# script track) closure: cross-domain tour example (Time+Input+
rem Ecs+Log+Camera+Props in one script, v0.9.0 final, ApiTable 39 unchanged)
rem -- gen.py --check (39 exports golden), full selftest replay (s0-s13,
rem M-SC0..SC10 non-regression), tour example compile check, then evidence
rem PNG regeneration from real output. Exit 0 = all green.
rem Viewer image evidence (tour-DX12/VULKAN.png, tour-sweep-*.png) is
rem maintained by the m-sc11 regression cases (baseline PNGs).
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
set "TOUR=%ROOT%\script\examples\tour"

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\DX12\nl-script.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === gen.py --check (golden byte-compare, 39 exports unchanged, exit 0 expected) ===
python "%ROOT%\tools\scriptapi\gen.py" --check > "%LOGS%gen-check.log" || exit /b 1
type "%LOGS%gen-check.log"

echo === selftest full replay (s0-s13, whole-track non-regression) ===
"%EXE%" selftest "%HELLO%" "%CONS%" "%ECSP%" "%BOOT%" "%TI%" "%CAM%" "%MODEL%" "%DRIVE%" "%PROPS%" > "%LOGS%selftest.log" || exit /b 1
type "%LOGS%selftest.log"

echo === check tour example (compile only, exit 0 expected) ===
"%EXE%" check "%TOUR%" > "%LOGS%check-tour.log" || exit /b 1
type "%LOGS%check-tour.log"

echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-script-evidence.py" m-sc11 || exit /b 1

echo === M-SC11 ACCEPTANCE: ALL GREEN ===
exit /b 0

@echo off
rem M-A track (action system) acceptance: deterministic selftests + sample dump
rem + evidence PNG regeneration. All output ASCII. Exit 0 = all green.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\nmake\anim-tools.exe"

if not exist "%EXE%" (
    echo [ERROR] anim-tools not built: build\nmake\anim-tools.exe
    echo         build via scripts\build-anim.bat anim-tools
    exit /b 1
)

echo === selftest curve ===
"%EXE%" selftest curve || exit /b 1
echo === selftest blend ===
"%EXE%" selftest blend || exit /b 1
echo === selftest statemachine ===
"%EXE%" selftest statemachine || exit /b 1
echo === selftest blendspace ===
"%EXE%" selftest blendspace || exit /b 1
echo === negative: unknown case (expect exit 2) ===
"%EXE%" selftest bogus >nul 2>&1
if not errorlevel 2 (
    echo [ERROR] unknown selftest case should exit 2
    exit /b 1
)
echo === sample dump + evidence PNG ===
"%EXE%" sample || exit /b 1
python "%ROOT%\tools\anim_evidence.py" || exit /b 1

echo === MA-TRACK ACCEPTANCE: ALL GREEN ===
exit /b 0

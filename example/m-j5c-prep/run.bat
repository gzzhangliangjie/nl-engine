@echo off
rem M-J5c-prep evidence regeneration (A-class track, tool deliverable).
rem Prereq: build/DX12/nanite-tools.exe current (scripts\build-m0.2.bat DX12).
setlocal enabledelayedexpansion
set EXE=%~dp0..\build\DX12\nanite-tools.exe
set OUT=%~dp0m-j5c-prep

if not exist "%EXE%" (
  echo [FAIL] missing %EXE%
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"

"%EXE%" selftest jobsync > "%OUT%\selftest-jobsync.log" 2>&1
if errorlevel 1 ( echo [FAIL] selftest jobsync & exit /b 1 )
findstr /c:"jobsync selftest: PASS" "%OUT%\selftest-jobsync.log" >nul || ( echo [FAIL] jobsync verdict & exit /b 1 )

"%EXE%" selftest jobs > "%OUT%\selftest-jobs.log" 2>&1
if errorlevel 1 ( echo [FAIL] selftest jobs & exit /b 1 )
findstr /c:"jobs selftest: PASS" "%OUT%\selftest-jobs.log" >nul || ( echo [FAIL] jobs verdict & exit /b 1 )

"%EXE%" selftest scheduler > "%OUT%\selftest-scheduler.log" 2>&1
if errorlevel 1 ( echo [FAIL] selftest scheduler & exit /b 1 )

"%EXE%" selftest jobgraph > "%OUT%\selftest-jobgraph.log" 2>&1
if errorlevel 1 ( echo [FAIL] selftest jobgraph & exit /b 1 )

"%EXE%" selftest unknown-suite > "%OUT%\negative-unknown.log" 2>&1
if errorlevel 0 if not errorlevel 1 ( echo [FAIL] unknown suite must exit non-zero & exit /b 1 )
findstr /c:"unknown suite" "%OUT%\negative-unknown.log" >nul || ( echo [FAIL] negative verdict & exit /b 1 )

python "%~dp0..\scripts\make-j5c-prep-evidence.py" || exit /b 1

echo [PASS] M-J5c-prep evidence regenerated (3 png + 5 logs)
exit /b 0

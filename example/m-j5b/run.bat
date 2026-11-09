@echo off
setlocal EnableDelayedExpansion
rem M-J5b acceptance: regenerate EVERY log from real tool output, then render
rem evidence PNGs from those logs via scripts/make-j5b-evidence.py.
rem A-track tool milestone (no kStage, no render regression). ASCII only;
rem !VAR! delayed expansion inside for blocks (M4.1 lesson).
rem Usage: example\m-j5b\run.bat   (exit 0 = all gates passed)

set "ROOT=%~dp0..\.."
set "BUILD=%ROOT%\build\DX12"
set "OUT=%~dp0"
set FAIL=0

if not exist "%BUILD%\nanite-tools.exe" echo missing nanite-tools.exe & exit /b 1

rem PRIMARY: new resident Scheduler selftest (11 checks).
"%BUILD%\nanite-tools.exe" selftest scheduler > "%OUT%selftest-scheduler.log" 2>&1
if errorlevel 1 set FAIL=1

rem REGRESSION GUARD: legacy ThreadPool 25-item suite must not regress.
"%BUILD%\nanite-tools.exe" selftest jobs > "%OUT%selftest-jobs.log" 2>&1
if errorlevel 1 set FAIL=1

rem NEGATIVE: unknown suite must be rejected with a non-zero exit.
"%BUILD%\nanite-tools.exe" selftest unknown-suite > "%OUT%negative-unknown.log" 2>&1
if errorlevel 1 (
  echo negative: OK ^(exit 1 as expected^)
) else (
  echo negative: FAIL ^(unexpectedly exited 0^)
  set FAIL=1
)

python "%ROOT%\scripts\make-j5b-evidence.py"
if errorlevel 1 set FAIL=1

echo.
if "!FAIL!"=="0" (
  echo M-J5b acceptance: PASS
  exit /b 0
)
echo M-J5b acceptance: FAIL
exit /b 1

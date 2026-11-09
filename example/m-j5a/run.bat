@echo off
setlocal EnableDelayedExpansion
rem M-J5a acceptance: regenerate EVERY log from real tool output, then render
rem evidence PNGs from those logs via scripts/make-j5a-evidence.py.
rem A-track tool milestone (no kStage, no render regression). ASCII only;
rem !VAR! delayed expansion inside for blocks (M4.1 lesson).
rem Usage: example\m-j5a\run.bat   (exit 0 = all gates passed)

set "ROOT=%~dp0..\.."
set "BUILD=%ROOT%\build\DX12"
set "OUT=%~dp0"
set FAIL=0

if not exist "%BUILD%\nanite-tools.exe" echo missing nanite-tools.exe & exit /b 1
if not exist "%BUILD%\material-graph-tools.exe" echo missing material-graph-tools.exe & exit /b 1

"%BUILD%\nanite-tools.exe" selftest jobgraph > "%OUT%selftest-jobgraph.log" 2>&1
if errorlevel 1 set FAIL=1
"%BUILD%\nanite-tools.exe" selftest jobs > "%OUT%selftest-jobs.log" 2>&1
if errorlevel 1 set FAIL=1
"%BUILD%\material-graph-tools.exe" compilepar > "%OUT%compilepar.log" 2>&1
if errorlevel 1 set FAIL=1
"%BUILD%\material-graph-tools.exe" check > "%OUT%check.log" 2>&1
if errorlevel 1 set FAIL=1

rem negative case: unknown suite must be rejected with a non-zero exit.
"%BUILD%\nanite-tools.exe" selftest unknown-suite > "%OUT%negative-unknown.log" 2>&1
if errorlevel 1 (
  echo negative: OK ^(exit 1 as expected^)
) else (
  echo negative: FAIL ^(unexpectedly exited 0^)
  set FAIL=1
)

python "%ROOT%\scripts\make-j5a-evidence.py"
if errorlevel 1 set FAIL=1

echo.
if "!FAIL!"=="0" (
  echo M-J5a acceptance: PASS
  exit /b 0
)
echo M-J5a acceptance: FAIL
exit /b 1

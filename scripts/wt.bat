@echo off
setlocal EnableExtensions
rem wt.bat -- single entry for worktree-scoped build/ctest/regression (2026-11-13).
rem cwd NEVER matters: the target tree is resolved from the track name via
rem tools\wt_gate.py locate (reads .wt_pool_busy.json at the main tree root),
rem then the copy of the tool INSIDE that tree is invoked. Wrong/mistyped
rem track names fail loudly instead of silently building the main repo.
rem
rem Usage:
rem   scripts\wt.bat <track> build      [DX12|VULKAN]   (default DX12)
rem   scripts\wt.bat <track> ctest      DX12|VULKAN [extra ctest args...]
rem   scripts\wt.bat <track> regression [regression.py args...]
rem Examples:
rem   scripts\wt.bat vsm build DX12
rem   scripts\wt.bat vsm ctest DX12
rem   scripts\wt.bat vsm regression --only perf-m1
rem
rem Implementation note: NO labels/shift inside parenthesized blocks (cmd
rem parse-time expansion trap). All loops/dispatch are flat goto flow.

set "TRACK=%~1"
set "OP=%~2"
set "WT="
rem Capture self paths BEFORE any shift: shift promotes %0 too, so %~dp0
rem AFTER a shift resolves against a call argument, not the script (real bug,
rem found 2026-11-13: %~dp0 became the cwd after two shifts).
set "SELFDIR=%~dp0"
if "%TRACK%"=="" goto :usage
if "%OP%"=="" goto :usage
shift
shift

rem -- collect remaining args: A1 = first (backend for build/ctest), REST = rest
set "A1="
set "REST="
:collect
if "%~1"=="" goto :resolved
if not defined A1 goto :take_a1
set "REST=%REST% %~1"
goto :shift_on
:take_a1
set "A1=%~1"
:shift_on
shift
goto :collect

:resolved
rem NOTE: %~dp0 does NOT survive shifts (see top) -- use SELFDIR. %VAR% env
rem expansion DOES work in the backquote body.
set "GATE=%SELFDIR%..\tools\wt_gate.py"
for /f "usebackq delims=" %%i in (`python "%GATE%" locate %TRACK%`) do set "WT=%%i"
if not defined WT goto :notrack
if not exist "%WT%\" goto :nowt

if /i "%OP%"=="build" goto :op_build
if /i "%OP%"=="ctest" goto :op_ctest
if /i "%OP%"=="regression" goto :op_regression
echo WT.BAT FAIL: unknown op "%OP%" (build ^| ctest ^| regression).
exit /b 4

:op_build
if "%A1%"=="" set "A1=DX12"
pushd "%WT%"
call "%WT%\scripts\build-m0.2.bat" %A1%%REST%
set "RC=%errorlevel%"
popd
exit /b %RC%

:op_regression
pushd "%WT%"
python "%WT%\tools\regression.py" %A1%%REST%
set "RC=%errorlevel%"
popd
exit /b %RC%

:op_ctest
if "%A1%"=="" (
  echo WT.BAT FAIL: ctest needs a backend: wt.bat %TRACK% ctest DX12^|VULKAN
  exit /b 4
)
set "CTEST=E:\UE_STUDY_2026\.build-tools\cmake-3.31.6-windows-x86_64\bin\ctest.exe"
if not exist "%CTEST%" set "CTEST=ctest"
pushd "%WT%"
"%CTEST%" --test-dir "%WT%\build\%A1%" -C Release --output-on-failure --repeat until-pass:3%REST%
set "RC=%errorlevel%"
popd
exit /b %RC%

:notrack
echo WT.BAT FAIL: no active worktree for track "%TRACK%" (details on stderr).
exit /b 4

:nowt
echo WT.BAT FAIL: worktree dir missing on disk: %WT%
exit /b 4

:usage
echo Usage: scripts\wt.bat ^<track^> build^|ctest^|regression [args...]
echo   build      : wt.bat ^<track^> build [DX12^|VULKAN]
echo   ctest      : wt.bat ^<track^> ctest DX12^|VULKAN [extra ctest args]
echo   regression : wt.bat ^<track^> regression [regression.py args]
echo Active tracks: see .wt_pool_busy.json or: python tools\wt_gate.py locate ^<track^>
exit /b 4

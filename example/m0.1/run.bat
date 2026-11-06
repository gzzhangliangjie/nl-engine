@echo off
setlocal
rem ==== M0.1 / s0-init successful cases ======================================
rem No GPU readback at this stage: the visible case is the real window + selftest.
rem usage: run.bat [all^|selftest^|window]
set "ROOT=%~dp0..\.."
set "DX=%ROOT%\build\showcase\s0-init\build\DX12\viewer.exe"
set "VK=%ROOT%\build\showcase\s0-init\build\VULKAN\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="window" goto window
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|selftest^|window]
exit /b 1

:all
call :selftest || exit /b 1
echo [manual] window case: run "example\m0.1\run.bat window", watch the window, press ESC.
exit /b 0

:selftest
echo === M0.1 selftest, both backend configs (7/7 each) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:window
echo === M0.1 empty window (1280x720, no GL context). Press ESC to quit. ===
"%DX%"
exit /b %errorlevel%

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Rebuild historical snapshots: python tools\showcase\rebuild.py
exit /b 1

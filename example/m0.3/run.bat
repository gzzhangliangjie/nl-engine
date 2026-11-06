@echo off
setlocal
rem ==== M0.3 / s0-triangle successful cases ==================================
rem One triangle.slang -> SPIR-V + DXIL; both backends draw the same triangle.
rem usage: run.bat [all^|triangle^|selftest]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\showcase\s0-triangle\build\DX12\viewer.exe"
set "VK=%ROOT%\build\showcase\s0-triangle\build\VULKAN\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="triangle" goto triangle
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|triangle^|selftest]
exit /b 1

:all
call :triangle || exit /b 1
call :selftest || exit /b 1
exit /b 0

:triangle
echo === M0.3 vertex-color triangle: DX12 + Vulkan, MD5 must match ===
"%DX%" --screenshot "%OUT%\m0.3-triangle-DX12.png" --frames 8 || exit /b 1
"%VK%" --screenshot "%OUT%\m0.3-triangle-VULKAN.png" --frames 8 || exit /b 1
echo output: %OUT%\m0.3-triangle-*.png
exit /b 0

:selftest
echo === M0.3 selftest (9/9 each backend) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Rebuild historical snapshots: python tools\showcase\rebuild.py
exit /b 1

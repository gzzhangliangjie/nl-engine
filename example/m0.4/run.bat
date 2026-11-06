@echo off
setlocal
rem ==== M0.4 / s0-model successful cases =====================================
rem Textured Suzanne, depth, culling on/off comparison, resize, selftest.
rem usage: run.bat [all^|suzanne^|inner^|resize^|selftest]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\dx12\viewer.exe"
set "VK=%ROOT%\build\vulkan\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="suzanne" goto suzanne
if /i "%CASE%"=="inner" goto inner
if /i "%CASE%"=="resize" goto resize
if /i "%CASE%"=="selftest" goto selftest
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|suzanne^|inner^|resize^|selftest]
exit /b 1

:all
call :suzanne || exit /b 1
call :inner || exit /b 1
call :resize || exit /b 1
call :selftest || exit /b 1
exit /b 0

:suzanne
echo === M0.4 textured Suzanne (yaw 12 pitch 9 dist 4.2 cull back) ===
"%DX%" --screenshot "%OUT%\m0.4-suzanne-DX12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
"%VK%" --screenshot "%OUT%\m0.4-suzanne-VULKAN.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --cull back || exit /b 1
echo output: %OUT%\m0.4-suzanne-*.png
exit /b 0

:inner
echo === M0.4 culling comparison: camera inside the model (dist 0.6) ===
"%DX%" --screenshot "%OUT%\m0.4-inner-DX12.png" --frames 4 --yaw 12 --pitch 9 --dist 0.6 --cull none || exit /b 1
"%VK%" --screenshot "%OUT%\m0.4-inner-VULKAN.png" --frames 4 --yaw 12 --pitch 9 --dist 0.6 --cull none || exit /b 1
"%DX%" --screenshot "%OUT%\m0.4-inner-cullback-DX12.png" --frames 4 --yaw 12 --pitch 9 --dist 0.6 --cull back || exit /b 1
"%VK%" --screenshot "%OUT%\m0.4-inner-cullback-VULKAN.png" --frames 4 --yaw 12 --pitch 9 --dist 0.6 --cull back || exit /b 1
echo compare: cull=none shows inner wall, cull=back shows background only
exit /b 0

:resize
echo === M0.4 resize 640x360 -^> 1024x768 (depth + framebuffer rebuilt) ===
"%DX%" --screenshot "%OUT%\m0.4-resize-DX12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --resize 1024x768 || exit /b 1
"%VK%" --screenshot "%OUT%\m0.4-resize-VULKAN.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --resize 1024x768 || exit /b 1
exit /b 0

:selftest
echo === M0.4 selftest (11/11 each backend) ===
"%DX%" --selftest || exit /b 1
"%VK%" --selftest || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build current tree first: scripts\build-m0.2.bat DX12 / VULKAN
exit /b 1

@echo off
setlocal enabledelayedexpansion
rem M-SP1 (mainline): world-space 2D sprite demo acceptance.
rem   normal - runNormal path, --sprite-demo on, multi-atlas per-bucket draw
rem            (8 floor tiles layer0 / enemy flipbook + 10 orbit props layer1 /
rem            5 premultiplied tint cards layer2). Frame-number pure data.
rem   scene  - runScene path, same deterministic data, renderFrame 6th arg wiring.
rem Baselines live in this directory (DX12 plain / VULKAN suffixed); re-runs are
rem compared by the regression harness (kind:image). Default-off zero-drift of
rem legacy baselines is verified by regression --only on preexisting cards.
rem Usage: run.bat normal^|scene   (exit 0 = ok, exit 1 = failure)
rem ASCII only.

set "ROOT=%~dp0..\.."
set "DX=%ROOT%\build\DX12\viewer.exe"
set "VK=%ROOT%\build\VULKAN\viewer.exe"
set "SCENE=%ROOT%\example\gl2\gl2-audio.scene"
set "OUT=%ROOT%\example\sprite-m1"

if not exist "%DX%" ( echo [ERROR] missing %DX% & exit /b 1 )
if not exist "%VK%" ( echo [ERROR] missing %VK% & exit /b 1 )

if "%~1"=="" goto :usage
if "%~1"=="normal" goto :normal
if "%~1"=="scene" goto :scene
goto :usage

:usage
echo usage: run.bat normal^|scene
exit /b 2

:normal
echo === M-SP1 normal: --sprite-demo multi-atlas per-bucket draw, both backends ===
"%DX%" --sprite-demo --no-hud --screenshot "%OUT%\normal-dx12.png" --frames 8 --size 960x540 --yaw 35 --pitch 12 --dist 16 --cull back > "%OUT%\normal-dx12.log" 2>&1
if errorlevel 1 ( echo [ERROR] normal DX12 FAILED & exit /b 1 )
"%VK%" --sprite-demo --no-hud --screenshot "%OUT%\normal-vulkan.png" --frames 8 --size 960x540 --yaw 35 --pitch 12 --dist 16 --cull back > "%OUT%\normal-vulkan.log" 2>&1
if errorlevel 1 ( echo [ERROR] normal VULKAN FAILED & exit /b 1 )
findstr /c:"frame timing" "%OUT%\normal-dx12.log" >nul
if errorlevel 1 ( echo [ERROR] normal DX12 no frame timing anchor & exit /b 1 )
findstr /c:"frame timing" "%OUT%\normal-vulkan.log" >nul
if errorlevel 1 ( echo [ERROR] normal VULKAN no frame timing anchor & exit /b 1 )
echo output: %OUT%\normal-*.png
exit /b 0

:scene
echo === M-SP1 scene: runScene path same deterministic sprite data, both backends ===
"%DX%" --mode scene --scene "%SCENE%" --sprite-demo --no-hud --screenshot "%OUT%\scene-dx12.png" --frames 8 --size 960x540 --yaw 12 --pitch 9 > "%OUT%\scene-dx12.log" 2>&1
if errorlevel 1 ( echo [ERROR] scene DX12 FAILED & exit /b 1 )
"%VK%" --mode scene --scene "%SCENE%" --sprite-demo --no-hud --screenshot "%OUT%\scene-vulkan.png" --frames 8 --size 960x540 --yaw 12 --pitch 9 > "%OUT%\scene-vulkan.log" 2>&1
if errorlevel 1 ( echo [ERROR] scene VULKAN FAILED & exit /b 1 )
findstr /c:"frame timing" "%OUT%\scene-dx12.log" >nul
if errorlevel 1 ( echo [ERROR] scene DX12 no frame timing anchor & exit /b 1 )
findstr /c:"frame timing" "%OUT%\scene-vulkan.log" >nul
if errorlevel 1 ( echo [ERROR] scene VULKAN no frame timing anchor & exit /b 1 )
echo output: %OUT%\scene-*.png
exit /b 0

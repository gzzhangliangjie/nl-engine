@echo off
setlocal enabledelayedexpansion
rem W1 M-W1.1 (feat/worldgen): textured city consumption acceptance. The
rem engine loads example/w0-m3-driven city.scene.engine (36 flat glbs with
rem embedded ambientCG CC0 facade textures, produced by scripts/city2engine.py
rem drive) via viewer --scene, shooting top + street screenshots per backend.
rem   top    - pitch -89 overhead: 36 regular building blocks, 3 texture tiers
rem   street - pitch -10 street level: facade windows clearly visible
rem PNG bytes are NOT fc-compared (offscreen timing may differ per run; the
rem acceptance is exit 0 + screenshot written + visual evidence).
rem Usage: run.bat   (exit 0 = ok)
rem ASCII only.

set "ROOT=%~dp0..\.."
set "DX=%ROOT%\build\DX12\viewer.exe"
set "VK=%ROOT%\build\VULKAN\viewer.exe"
set "SCENE=%ROOT%\example\w1-m0\city.scene.engine"
set "OUT=%ROOT%\example\w1-m1"

if not exist "%DX%" ( echo [ERROR] missing %DX% & exit /b 1 )
if not exist "%VK%" ( echo [ERROR] missing %VK% & exit /b 1 )
if not exist "%SCENE%" ( echo [ERROR] missing %SCENE% & exit /b 1 )

:top
echo === M-W1.1 top: overhead 36 blocks (both backends) ===
"%DX%" --scene "%SCENE%" --no-hud --screenshot "%OUT%\city_top.png" --frames 8 --size 960x540 --yaw 0 --pitch -89 --dist 48 > "%OUT%\city_top-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] top DX12 FAILED & exit /b 1 )
findstr /c:"screenshot written" "%OUT%\city_top-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] top DX12 no screenshot & exit /b 1 )
"%VK%" --scene "%SCENE%" --no-hud --screenshot "%OUT%\city_top-VULKAN.png" --frames 8 --size 960x540 --yaw 0 --pitch -89 --dist 48 > "%OUT%\city_top-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] top VULKAN FAILED & exit /b 1 )
findstr /c:"screenshot written" "%OUT%\city_top-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] top VULKAN no screenshot & exit /b 1 )

:street
echo === M-W1.1 street: facade windows at street level (both backends) ===
"%DX%" --scene "%SCENE%" --no-hud --screenshot "%OUT%\city_street.png" --frames 8 --size 960x540 --yaw 28 --pitch -10 --dist 30 > "%OUT%\city_street-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] street DX12 FAILED & exit /b 1 )
findstr /c:"screenshot written" "%OUT%\city_street-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] street DX12 no screenshot & exit /b 1 )
"%VK%" --scene "%SCENE%" --no-hud --screenshot "%OUT%\city_street-VULKAN.png" --frames 8 --size 960x540 --yaw 28 --pitch -10 --dist 30 > "%OUT%\city_street-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] street VULKAN FAILED & exit /b 1 )
findstr /c:"screenshot written" "%OUT%\city_street-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] street VULKAN no screenshot & exit /b 1 )

echo output: %OUT%\city_top*.png + city_street*.png
exit /b 0

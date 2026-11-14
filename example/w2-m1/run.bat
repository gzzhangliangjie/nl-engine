@echo off
rem ---- w2-m1 run.bat : M-W2.1 variant-pool consumption acceptance
rem ---- city2engine drive -> engine assets + viewer --scene renders
rem ---- (variant skyline: single / setback / podium+tower / L-shape) per backend
rem ---- acceptance is exit 0 + screenshot written + visual evidence
setlocal
set "ROOT=%~dp0..\.."
set "DX=%ROOT%\build\DX12\viewer.exe"
set "VK=%ROOT%\build\VULKAN\viewer.exe"
set "DRIVER=%ROOT%\scripts\city2engine.py"
set "NT=%ROOT%\build\DX12\nanite-tools.exe"
set "SRC=%ROOT%\example\w0-m3"
set "OUT=%~dp0"

echo === 1 drive seed=12345 size=6 ===
python "%DRIVER%" drive --src "%SRC%" --seed 12345 --size 6 --nanite "%NT%" --scene-out "%OUT%city.scene.engine" > "%OUT%drive.log" 2>&1
if errorlevel 1 (
    echo [ERROR] drive FAILED
    type "%OUT%drive.log"
    exit /b 1
)
findstr /c:"city2engine drive OK: blocks=36 glb=36 nmesh_suites=36 dag=36 scene_refs=36" "%OUT%drive.log" >nul || (
    echo [ERROR] missing drive stats anchor
    exit /b 1
)
echo drive OK (36 variant-pool blocks -> glb + nanite suite + city.scene.engine)

echo === 2 top renders (pitch -89) ===
"%DX%" --scene "%OUT%city.scene.engine" --no-hud --screenshot "%OUT%city_top.png" --frames 8 --size 960x540 --yaw 0 --pitch -89 --dist 48 > "%OUT%city_top-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] top DX12 FAILED & exit /b 1 )
findstr /c:"screenshot written" "%OUT%city_top-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] top DX12 no screenshot & exit /b 1 )
"%VK%" --scene "%OUT%city.scene.engine" --no-hud --screenshot "%OUT%city_top-VULKAN.png" --frames 8 --size 960x540 --yaw 0 --pitch -89 --dist 48 > "%OUT%city_top-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] top VULKAN FAILED & exit /b 1 )
findstr /c:"screenshot written" "%OUT%city_top-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] top VULKAN no screenshot & exit /b 1 )
echo top renders OK (DX12 + VULKAN)

echo === 3 street renders (yaw 28 pitch -10 dist 30) ===
"%DX%" --scene "%OUT%city.scene.engine" --no-hud --screenshot "%OUT%city_street.png" --frames 8 --size 960x540 --yaw 28 --pitch -10 --dist 30 > "%OUT%city_street-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] street DX12 FAILED & exit /b 1 )
findstr /c:"screenshot written" "%OUT%city_street-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] street DX12 no screenshot & exit /b 1 )
"%VK%" --scene "%OUT%city.scene.engine" --no-hud --screenshot "%OUT%city_street-VULKAN.png" --frames 8 --size 960x540 --yaw 28 --pitch -10 --dist 30 > "%OUT%city_street-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] street VULKAN FAILED & exit /b 1 )
findstr /c:"screenshot written" "%OUT%city_street-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] street VULKAN no screenshot & exit /b 1 )
echo street renders OK (DX12 + VULKAN)

echo.
echo w2-m1 run.bat ALL GREEN
endlocal

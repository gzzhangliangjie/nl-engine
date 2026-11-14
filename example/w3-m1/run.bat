@echo off
rem M-W3.1 street-asset consumption gate: city2engine drive (vendored glb
rem assembly, A1) + viewer --scene street-asset renders (top + street).
setlocal
set "ROOT=%~dp0..\.."
set "SCENE=%~dp0city.scene.engine"
set "OUT=%~dp0"

echo [seg1] drive with vendored assembly
python "%ROOT%\scripts\city2engine.py" drive --src "%ROOT%\example\w0-m3" --seed 12345 --size 6 --nanite "%ROOT%\build\DX12\nanite-tools.exe" --scene-out "%SCENE%" > "%OUT%drive.log" 2>&1
findstr /c:"city2engine drive OK: blocks=36 glb=36 nmesh_suites=36 dag=36 scene_refs=36 vendored=21" "%OUT%drive.log" >nul || (
    echo [ERROR] drive anchor missing (vendored=21)
    exit /b 1
)

echo [seg2] viewer renders (top + street, both backends)
for %%B in (DX12 VULKAN) do (
    "%ROOT%\build\%%B\viewer.exe" --scene "%SCENE%" --no-hud --screenshot "%OUT%city_top.png" --frames 8 --size 960x540 --yaw 0 --pitch -89 --dist 48 > "%OUT%city_top-%%B.log" 2>&1
    findstr /c:"screenshot written" "%OUT%city_top-%%B.log" >nul || ( echo [ERROR] top %%B shot missing & exit /b 1 )
    "%ROOT%\build\%%B\viewer.exe" --scene "%SCENE%" --no-hud --screenshot "%OUT%city_street.png" --frames 8 --size 960x540 --yaw 28 --pitch -10 --dist 30 > "%OUT%city_street-%%B.log" 2>&1
    findstr /c:"screenshot written" "%OUT%city_street-%%B.log" >nul || ( echo [ERROR] street %%B shot missing & exit /b 1 )
)
findstr /c:"objects=315" "%OUT%city_street-DX12.log" >nul || (
    echo [ERROR] scene object count != 315 (36+188+42+49)
    exit /b 1
)

echo w3-m1 ALL GREEN
endlocal

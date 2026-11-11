@echo off
rem P8 M-N3 panel supplement: viewer NavPanel dynamic-obstacle loop.
rem A = tiled bake + add obstacle, B = add + clear, C = solo zero-regression,
rem D = screenshot determinism, E = solo must reject obstacle. ASCII only.
rem Usage: run.bat [DX12|VULKAN]   Exit 0 = ALL GREEN.
setlocal
set "ROOT=%~dp0..\.."
set "BE=DX12"
if not "%~1"=="" set "BE=%~1"
set "EXE=%ROOT%\build\%BE%\viewer.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] viewer not built: build\%BE%\viewer.exe
    echo         build via scripts\build-m0.2.bat %BE%
    exit /b 1
)

echo === A: tiled bake + add obstacle ===
"%EXE%" --nav-model HizScene.glb --nav-panel --console-exec "nav.obsPanel 1" --console-exec "nav.tiled 1" --console-exec "nav.bake" --console-exec "nav.obsAdd" --frames 6 --size 1280x720 --screenshot "%D%obstacle-panel.png" > "%D%obstacle.log" 2>&1
if errorlevel 1 (type "%D%obstacle.log" & echo [ERROR] viewer A FAILED & exit /b 1)
findstr /c:"nav-panel: bake ok mode=tiled polys=4 tiles=2" "%D%obstacle.log" >nul || (echo [ERROR] A tiled bake anchor missing & exit /b 1)
findstr /c:"nav-panel: path OK (2 wp) polys=4 obstacles=0" "%D%obstacle.log" >nul || (echo [ERROR] A before-path anchor missing & exit /b 1)
findstr /c:"nav-panel: path UNREACHABLE (0 wp) polys=3 obstacles=1" "%D%obstacle.log" >nul || (echo [ERROR] A blocked-path anchor missing & exit /b 1)
findstr /c:"nav-panel: obstacle added ref=65536" "%D%obstacle.log" >nul || (echo [ERROR] A add anchor missing & exit /b 1)
findstr /c:"deviceErrors=0" "%D%obstacle.log" >nul || (echo [ERROR] A device errors & exit /b 1)
if not exist "%D%obstacle-panel.png" (echo [ERROR] missing obstacle-panel.png & exit /b 1)
echo A OK : polys 4-^>3, path OK-^>UNREACHABLE, red obstacle circle in canvas

echo === B: add + clear restores path point-by-point ===
"%EXE%" --nav-model HizScene.glb --nav-panel --console-exec "nav.obsPanel 1" --console-exec "nav.tiled 1" --console-exec "nav.bake" --console-exec "nav.obsAdd" --console-exec "nav.obsClear" --frames 6 --size 1280x720 --screenshot "%D%cleared-panel.png" > "%D%cleared.log" 2>&1
if errorlevel 1 (type "%D%cleared.log" & echo [ERROR] viewer B FAILED & exit /b 1)
findstr /c:"nav-panel: obstacles cleared polys=4 left=0" "%D%cleared.log" >nul || (echo [ERROR] B clear anchor missing & exit /b 1)
findstr /c:"nav-panel: path OK (2 wp) polys=4 obstacles=0" "%D%cleared.log" >nul || (echo [ERROR] B restored path anchor missing & exit /b 1)
findstr /c:"deviceErrors=0" "%D%cleared.log" >nul || (echo [ERROR] B device errors & exit /b 1)
if not exist "%D%cleared-panel.png" (echo [ERROR] missing cleared-panel.png & exit /b 1)
echo B OK : obstacles 1-^>0, polys 3-^>4, path restored

echo === C: solo default keeps M-N2 five-stage path untouched ===
"%EXE%" --nav-panel --console-exec "nav.bake" --console-exec "nav.stage 4" --frames 6 --size 1280x720 --screenshot "%D%solo-panel.png" > "%D%solo.log" 2>&1
if errorlevel 1 (type "%D%solo.log" & echo [ERROR] viewer C FAILED & exit /b 1)
findstr /c:"nav-panel: bake ok mode=solo" "%D%solo.log" >nul || (echo [ERROR] C solo bake anchor missing & exit /b 1)
findstr /c:"nav.stage = 4" "%D%solo.log" >nul || (echo [ERROR] C stage cvar anchor missing & exit /b 1)
findstr /c:"debug stage failed" "%D%solo.log" >nul && (echo [ERROR] C rc debug stage broken & exit /b 1)
findstr /c:"deviceErrors=0" "%D%solo.log" >nul || (echo [ERROR] C device errors & exit /b 1)
echo C OK : solo bake + rc debug stage still available

echo === D: screenshot determinism byte-identical ===
"%EXE%" --nav-model HizScene.glb --nav-panel --console-exec "nav.obsPanel 1" --console-exec "nav.tiled 1" --console-exec "nav.bake" --console-exec "nav.obsAdd" --frames 6 --size 1280x720 --screenshot "%D%obstacle-panel-b.png" > "%D%determinism.log" 2>&1
if errorlevel 1 (echo [ERROR] viewer D FAILED & exit /b 1)
fc /b "%D%obstacle-panel.png" "%D%obstacle-panel-b.png" >nul || (echo [ERROR] determinism: PNGs differ & exit /b 1)
echo D OK : obstacle-panel.png == second run byte-identical
del "%D%obstacle-panel-b.png"
del "%D%determinism.log"

echo === E: solo mesh must reject dynamic obstacle ===
"%EXE%" --nav-panel --console-exec "nav.bake" --console-exec "nav.obsAdd" --frames 4 --size 960x540 --screenshot "%D%reject.png" > "%D%reject.log" 2>&1
if errorlevel 1 (type "%D%reject.log" & echo [ERROR] viewer E FAILED & exit /b 1)
findstr /c:"nav-panel: obstacle rejected" "%D%reject.log" >nul || (echo [ERROR] E rejection anchor missing & exit /b 1)
echo E OK : solo rejected obstacle, no silent no-op

echo === NAV-N3-PANEL ACCEPTANCE: ALL GREEN ===
exit /b 0

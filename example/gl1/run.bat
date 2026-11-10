@echo off
setlocal
rem ==== P8 M-GL1 / gl-m1 successful cases ====================================
rem GameClock dual-frame model in the viewer: pause freezes simTime (render
rem frames keep running), timeScale multiplies the logic clock feed.
rem Evidence = 8 PNGs + MD5 cross-checks (check.py):
rem   pause120 == pause60     -> frozen across render frames (HUD still alive)
rem   pause120 == anim0 ref   -> paused bind pose == pinned t=0 (cross-path)
rem   scale2(dx16.7) == ref(dt33.4) -> identical accumulator trajectory
rem   all runs == committed baselines in example\gl1 (reproducibility)
rem usage: run.bat [all^|pause^|scale]
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\build\showcase\runs"
if not exist "%OUT%" mkdir "%OUT%"
set "DX=%ROOT%\build\DX12\viewer.exe"
set "VK=%ROOT%\build\VULKAN\viewer.exe"
call :check "%DX%" || exit /b 1
call :check "%VK%" || exit /b 1
set "CASE=%~1"
if "%CASE%"=="" set "CASE=all"
if /i "%CASE%"=="pause" goto pause
if /i "%CASE%"=="scale" goto scale
if /i "%CASE%"=="all" goto all
echo usage: run.bat [all^|pause^|scale]
exit /b 1

:all
call :pause || exit /b 1
call :scale || exit /b 1
call :verify || exit /b 1
exit /b 0

:pause
echo === M-GL1 pause: logic clock frozen, render frames + HUD keep running ===
"%DX%" --mode skin --hud --screenshot "%OUT%\gl1-pause120-DX12.png" --frames 120 --fixed-dt 16.7 --console-exec "game.pause" || exit /b 1
"%DX%" --mode skin --hud --screenshot "%OUT%\gl1-pause60-DX12.png"  --frames 60  --fixed-dt 16.7 --console-exec "game.pause" || exit /b 1
"%DX%" --mode skin --hud --screenshot "%OUT%\gl1-anim0-DX12.png"    --frames 120 --anim-time 0 || exit /b 1
"%VK%" --mode skin --hud --screenshot "%OUT%\gl1-pause120-VULKAN.png" --frames 120 --fixed-dt 16.7 --console-exec "game.pause" || exit /b 1
"%VK%" --mode skin --hud --screenshot "%OUT%\gl1-pause60-VULKAN.png"  --frames 60  --fixed-dt 16.7 --console-exec "game.pause" || exit /b 1
"%VK%" --mode skin --hud --screenshot "%OUT%\gl1-anim0-VULKAN.png"    --frames 120 --anim-time 0 || exit /b 1
echo output: %OUT%\gl1-pause*.png %OUT%\gl1-anim0-*.png
exit /b 0

:scale
echo === M-GL1 timeScale 2: scaled feed 16.7x2 == direct feed 33.4 (per frame) ===
rem --frames 100: capture frame lands at 200 ticks = 3.3333s (mid walk-cycle,
rem avoids the 240-tick=4.0s wrap-to-t0 coincidence of a 120-frame run).
"%DX%" --mode skin --hud --screenshot "%OUT%\gl1-scale2-DX12.png"   --frames 100 --fixed-dt 16.7 --console-exec "game.timeScale 2" || exit /b 1
"%DX%" --mode skin --hud --screenshot "%OUT%\gl1-scale2ref-DX12.png" --frames 100 --fixed-dt 33.4 || exit /b 1
"%VK%" --mode skin --hud --screenshot "%OUT%\gl1-scale2-VULKAN.png"   --frames 100 --fixed-dt 16.7 --console-exec "game.timeScale 2" || exit /b 1
"%VK%" --mode skin --hud --screenshot "%OUT%\gl1-scale2ref-VULKAN.png" --frames 100 --fixed-dt 33.4 || exit /b 1
echo output: %OUT%\gl1-scale2*.png
exit /b 0

:verify
echo === MD5 cross-checks (check.py) ===
python "%~dp0check.py" || exit /b 1
exit /b 0

:check
if exist "%~1" exit /b 0
echo [ERROR] viewer.exe missing: %~1
echo Build first: scripts\build-m0.2.bat DX12 / scripts\build-m0.2.bat VULKAN
exit /b 1

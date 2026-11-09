@echo off
setlocal enabledelayedexpansion
rem P8 M-GL2 (feat/gameloop): physics/ECS/audio all driven by the logic-frame
rem clock. Four runs on gl2-audio.scene (spin parents + E1a orbit children):
rem   move  - 100 ticks, two spatial sources bound to orbit entities;
rem           readback must equal entity world pos (equal=1) and positions
rem           must be ON the orbit circles (radius invariant, see verify.py).
rem   pause - game.pause before bind: clock frozen (simTime=0, ticks=0),
rem           positions frozen at bind-time values (UE-style: audio mix frozen
rem           too via audio.freeze coupling, M-GL2 user decision B).
rem Baseline drift guards for the migrated cases are NOT here - ecs-parallel /
rem phys-p3 / phys-p4 run.bat re-runs must leave their PNGs byte-identical
rem (--fixed-dt 16.7 keeps total tick count unchanged).
rem Usage: run.bat   (exit 0 = ok)
rem ASCII only.

set "ROOT=%~dp0..\.."
set "DX=%ROOT%\build\DX12\viewer.exe"
set "VK=%ROOT%\build\VULKAN\viewer.exe"
set "SCENE=%ROOT%\example\gl2\gl2-audio.scene"
set "WAV=%ROOT%\assets\audio\test-tone.wav"
set "OUT=%ROOT%\example\gl2"
set "PY=%ROOT%\example\gl2\verify.py"

if not exist "%DX%" ( echo [ERROR] missing %DX% & exit /b 1 )
if not exist "%VK%" ( echo [ERROR] missing %VK% & exit /b 1 )

:move
echo === M-GL2 move: 100 ticks, sources bound to orbit entities (both backends) ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "audio.play %WAV% orb-a" --console-exec "audio.play %WAV% orb-b" --screenshot "%OUT%\gl2-move-DX12.png" > "%OUT%\gl2-move-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] move DX12 FAILED & exit /b 1 )
"%VK%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "audio.play %WAV% orb-a" --console-exec "audio.play %WAV% orb-b" --screenshot "%OUT%\gl2-move-VULKAN.png" > "%OUT%\gl2-move-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] move VULKAN FAILED & exit /b 1 )
findstr /c:"equal=1" "%OUT%\gl2-move-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] move DX12 readback not equal & exit /b 1 )
findstr /c:"equal=1" "%OUT%\gl2-move-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] move VULKAN readback not equal & exit /b 1 )
findstr /c:"simTime=1.66667 ticks=100 paused=0" "%OUT%\gl2-move-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] move DX12 clock state wrong & exit /b 1 )
echo output: %OUT%\gl2-move-*.png

:pause
echo === M-GL2 pause: clock frozen, positions frozen at bind values (both backends) ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "game.pause" --console-exec "audio.play %WAV% orb-a" --console-exec "audio.play %WAV% orb-b" --screenshot "%OUT%\gl2-pause-DX12.png" > "%OUT%\gl2-pause-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] pause DX12 FAILED & exit /b 1 )
"%VK%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "game.pause" --console-exec "audio.play %WAV% orb-a" --console-exec "audio.play %WAV% orb-b" --screenshot "%OUT%\gl2-pause-VULKAN.png" > "%OUT%\gl2-pause-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] pause VULKAN FAILED & exit /b 1 )
findstr /c:"simTime=0 ticks=0 paused=1" "%OUT%\gl2-pause-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] pause DX12 clock not frozen & exit /b 1 )
findstr /c:"simTime=0 ticks=0 paused=1" "%OUT%\gl2-pause-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] pause VULKAN clock not frozen & exit /b 1 )
echo output: %OUT%\gl2-pause-*.png

:verify
echo === M-GL2 verify: independent python twin on the four logs ===
python "%PY%" "%OUT%\gl2-move-DX12.log" "%OUT%\gl2-move-VULKAN.log" "%OUT%\gl2-pause-DX12.log" "%OUT%\gl2-pause-VULKAN.log"
if errorlevel 1 ( echo [ERROR] verify.py FAILED & exit /b 1 )

echo [PASS] M-GL2 evidence regenerated (4 png + 4 logs, verify twin all-green)
exit /b 0

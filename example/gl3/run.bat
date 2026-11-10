@echo off
setlocal enabledelayedexpansion
rem P8 M-GL3 (feat/gameloop): render-frame interpolation (alpha = accumulator/step,
 rem bevy Fixed overstep/timestep same-aspect; render-only, never written back).
 rem Scene = gl2-audio.scene reused (spin parents + E1a orbit children, no audio).
 rem   equal - interp OFF vs ON, fixed-dt 16.7, 100 frames: screenshot render pins
 rem           alpha to 0 (capture = tick-boundary state) => PNGs byte-identical.
 rem   mid   - fixed-dt 25 => per-frame ticks [1,2,1] (python twin re-derives the
 rem           accumulator), capture frame alpha = 0.5: probe logs prev/curr/
 rem           interp/quats; verify.py recomputes lerp/slerp AND the orbit world
 rem           positions from the SpinSystem formula (independent twin).
 rem   pause - game.pause + interp on: prev == curr == interp, alpha = 0.
 rem   smoke - game.interp on/off toggle + bad arg prints usage line.
 rem Usage: run.bat   (exit 0 = ok)
 rem ASCII only.

set "ROOT=%~dp0..\.."
set "DX=%ROOT%\build\DX12\viewer.exe"
set "VK=%ROOT%\build\VULKAN\viewer.exe"
set "SCENE=%ROOT%\example\gl2\gl2-audio.scene"
set "OUT=%ROOT%\example\gl3"
set "PY=%ROOT%\example\gl3\verify.py"

if not exist "%DX%" ( echo [ERROR] missing %DX% & exit /b 1 )
if not exist "%VK%" ( echo [ERROR] missing %VK% & exit /b 1 )

:equal
echo === M-GL3 equal: interp OFF vs ON byte-identical PNGs (both backends) ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --screenshot "%OUT%\gl3-equal-off-DX12.png" > "%OUT%\gl3-equal-off-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] equal off DX12 FAILED & exit /b 1 )
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --interp --screenshot "%OUT%\gl3-equal-on-DX12.png" > "%OUT%\gl3-equal-on-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] equal on DX12 FAILED & exit /b 1 )
"%VK%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --screenshot "%OUT%\gl3-equal-off-VULKAN.png" > "%OUT%\gl3-equal-off-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] equal off VULKAN FAILED & exit /b 1 )
"%VK%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --interp --screenshot "%OUT%\gl3-equal-on-VULKAN.png" > "%OUT%\gl3-equal-on-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] equal on VULKAN FAILED & exit /b 1 )
echo output: %OUT%\gl3-equal-*.png

:mid
echo === M-GL3 mid: fixed-dt 25, capture at alpha=0.5, twin-checked probes ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 25 --frames 3 --size 960x540 --no-hud --yaw 12 --pitch 9 --interp --console-exec "game.status" --screenshot "%OUT%\gl3-mid-DX12.png" > "%OUT%\gl3-mid-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] mid DX12 FAILED & exit /b 1 )
"%VK%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 25 --frames 3 --size 960x540 --no-hud --yaw 12 --pitch 9 --interp --console-exec "game.status" --screenshot "%OUT%\gl3-mid-VULKAN.png" > "%OUT%\gl3-mid-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] mid VULKAN FAILED & exit /b 1 )
echo output: %OUT%\gl3-mid-*.png

:pause
echo === M-GL3 pause: clock frozen, interp state frozen too (both backends) ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 50 --size 960x540 --no-hud --yaw 12 --pitch 9 --interp --console-exec "game.pause" --console-exec "game.status" --screenshot "%OUT%\gl3-pause-DX12.png" > "%OUT%\gl3-pause-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] pause DX12 FAILED & exit /b 1 )
"%VK%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 50 --size 960x540 --no-hud --yaw 12 --pitch 9 --interp --console-exec "game.pause" --console-exec "game.status" --screenshot "%OUT%\gl3-pause-VULKAN.png" > "%OUT%\gl3-pause-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] pause VULKAN FAILED & exit /b 1 )
findstr /c:"paused=1" "%OUT%\gl3-pause-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] pause DX12 not registered & exit /b 1 )
findstr /c:"paused=1" "%OUT%\gl3-pause-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] pause VULKAN not registered & exit /b 1 )
echo output: %OUT%\gl3-pause-*.png

:smoke
echo === M-GL3 smoke: game.interp toggle + bad arg negative (console errors fail acceptance) ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 2 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "game.interp" --console-exec "game.interp off" --console-exec "game.status" --screenshot "%OUT%\gl3-smoke-DX12.png" > "%OUT%\gl3-smoke-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] smoke DX12 FAILED & exit /b 1 )
findstr /c:"game.interp -> on" "%OUT%\gl3-smoke-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] smoke: toggle should print on & exit /b 1 )
findstr /c:"game.interp -> off" "%OUT%\gl3-smoke-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] smoke: set off failed & exit /b 1 )
rem negative: a failed console command trips the acceptance counter -> nonzero exit
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 2 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "game.interp bogus" --screenshot "%OUT%\gl3-smoke-bad-DX12.png" > "%OUT%\gl3-smoke-bad-DX12.log" 2>&1
if not errorlevel 1 ( echo [ERROR] smoke: bad arg should exit nonzero & exit /b 1 )
findstr /c:"usage: game.interp [on|off]" "%OUT%\gl3-smoke-bad-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] smoke: bad arg usage line missing & exit /b 1 )

:verify
echo === M-GL3 verify: independent python twin (PNG identity + clock/pos/quat twins) ===
python "%PY%" "%OUT%\gl3-equal-off-DX12.log" "%OUT%\gl3-equal-on-DX12.log" "%OUT%\gl3-equal-off-VULKAN.log" "%OUT%\gl3-equal-on-VULKAN.log" "%OUT%\gl3-equal-off-DX12.png" "%OUT%\gl3-equal-on-DX12.png" "%OUT%\gl3-equal-off-VULKAN.png" "%OUT%\gl3-equal-on-VULKAN.png" "%OUT%\gl3-mid-DX12.log" "%OUT%\gl3-mid-VULKAN.log" "%OUT%\gl3-pause-DX12.log" "%OUT%\gl3-pause-VULKAN.log"
if errorlevel 1 ( echo [ERROR] verify.py FAILED & exit /b 1 )

echo [PASS] M-GL3 evidence regenerated (10 png + 9 logs, verify twin all-green)
exit /b 0

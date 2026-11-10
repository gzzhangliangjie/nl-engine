@echo off
rem M-GL5 tick-group registry acceptance: scene order / phys order / negative, both backends.
rem Run from anywhere: example\gl5\run.bat  (needs viewer.exe for both backends).
rem Exit code: 0 = ALL GREEN; nonzero = failed (findstr / verify.py).
setlocal
set "ROOT=E:/UE_STUDY_2026/nl-engine-gameloop"
set "DX=%ROOT%/build/DX12/viewer.exe"
set "VK=%ROOT%/build/VULKAN/viewer.exe"
set "SCENE=%ROOT%/example/gl5/gl5-orbit.scene"
set "OUT=%ROOT%/example/gl5"
set "VERIFY=%ROOT%/example/gl5/verify.py"

if not exist "%DX%" ( echo [ERROR] DX12 viewer.exe missing & exit /b 1 )
if not exist "%VK%" ( echo [ERROR] VULKAN viewer.exe missing & exit /b 1 )

echo === M-GL5 scene-groups: tickgroups order in scene mode (Pre on / Physics off / Post on / PostWork on) ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 5 --size 960x540 --no-hud --console-exec "game.tickgroups" --screenshot "%OUT%\gl5-scene-DX12.png" > "%OUT%\gl5-scene-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] scene DX12 FAILED & exit /b 1 )
findstr /c:"game.tickgroups: order PrePhysics(on) -> Physics(off) -> PostPhysics(on) -> PostUpdateWork(on)" "%OUT%\gl5-scene-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] scene DX12: tickgroups order line missing & exit /b 1 )

"%VK%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 5 --size 960x540 --no-hud --console-exec "game.tickgroups" --screenshot "%OUT%\gl5-scene-VULKAN.png" > "%OUT%\gl5-scene-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] scene VULKAN FAILED & exit /b 1 )
findstr /c:"game.tickgroups: order PrePhysics(on) -> Physics(off) -> PostPhysics(on) -> PostUpdateWork(on)" "%OUT%\gl5-scene-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] scene VULKAN: tickgroups order line missing & exit /b 1 )

echo === M-GL5 phys-groups: Physics group registered under --physics-debug ===
"%DX%" --physics-debug --fixed-dt 16.7 --frames 5 --size 960x540 --no-hud --console-exec "game.tickgroups" --screenshot "%OUT%\gl5-phys-DX12.png" > "%OUT%\gl5-phys-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] phys DX12 FAILED & exit /b 1 )
findstr /c:"game.tickgroups: order PrePhysics(off) -> Physics(on) -> PostPhysics(off) -> PostUpdateWork(off)" "%OUT%\gl5-phys-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] phys DX12: tickgroups order line missing & exit /b 1 )

"%VK%" --physics-debug --fixed-dt 16.7 --frames 5 --size 960x540 --no-hud --console-exec "game.tickgroups" --screenshot "%OUT%\gl5-phys-VULKAN.png" > "%OUT%\gl5-phys-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] phys VULKAN FAILED & exit /b 1 )
findstr /c:"game.tickgroups: order PrePhysics(off) -> Physics(on) -> PostPhysics(off) -> PostUpdateWork(off)" "%OUT%\gl5-phys-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] phys VULKAN: tickgroups order line missing & exit /b 1 )

echo === M-GL5 negative: unknown command game.tickgroup must exit 1 ===
"%DX%" --physics-debug --fixed-dt 16.7 --frames 2 --size 960x540 --no-hud --console-exec "game.tickgroup" --screenshot "%OUT%\gl5-neg.png" > "%OUT%\gl5-neg.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative: viewer should exit 1 but exited 0 & exit /b 1 )
findstr /c:"unknown command: game.tickgroup" "%OUT%\gl5-neg.log" >nul
if errorlevel 1 ( echo [ERROR] negative: expected unknown-command line missing & exit /b 1 )

"%VK%" --physics-debug --fixed-dt 16.7 --frames 2 --size 960x540 --no-hud --console-exec "game.tickgroup" --screenshot "%OUT%\gl5-neg-VULKAN.png" > "%OUT%\gl5-neg-VULKAN.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative VK: viewer should exit 1 but exited 0 & exit /b 1 )
findstr /c:"unknown command: game.tickgroup" "%OUT%\gl5-neg-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] negative VK: expected unknown-command line missing & exit /b 1 )

echo === M-GL5 verify twin ===
python "%VERIFY%" "%OUT%\gl5-scene-DX12.log" "%OUT%\gl5-scene-VULKAN.log" "%OUT%\gl5-phys-DX12.log" "%OUT%\gl5-phys-VULKAN.log" "%OUT%\gl5-neg.log" "%OUT%\gl5-neg-VULKAN.log"
if errorlevel 1 ( echo [ERROR] verify twin FAILED & exit /b 1 )

echo M-GL5 ALL GREEN
endlocal
exit /b 0

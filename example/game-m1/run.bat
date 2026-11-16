@echo off
rem M-GAME1 gate (game.exe AI traffic: TrafficCarSystem + signal lamps).
rem Runs selftest / dual-backend traffic baseline with impact+odometer anchors /
rem determinism rerun / negative (--traffic bad value) / M-GAME0 no-traffic regression.
setlocal
set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "GDX=%ROOT%\build\DX12\game.exe"
set "GVK=%ROOT%\build\VULKAN\game.exe"

echo [seg1] game selftest
"%GDX%" --selftest > "%OUT%selftest.log" 2>&1
findstr /c:"game-m0 selftest: PASS" "%OUT%selftest.log" >nul || (
    echo [ERROR] game selftest not PASS
    exit /b 1
)

echo [seg2] traffic baseline DX12
"%GDX%" --city "%ROOT%\example\cg2-m0" --traffic 1 --cam free --cam-at -9.5,-24.1 --dist 40 --pitch -20 --yaw 0 --seed 1 --screenshot "%OUT%game-street.png" --screenshot-frame 8 --frames 16 --steps 240 --size 960x540 > "%OUT%run-dx.log" 2>&1
findstr /c:"game: traffic ready: cars=8 lanes=500 signal lamps=401 seed=12345" "%OUT%run-dx.log" >nul || ( echo [ERROR] DX traffic ready anchor & exit /b 1 )
findstr /c:"GAME_IMPACT frame_hits=1 owner=113 strength=4.7787 impulse=7167.9883 pos=(87.16,4.50,3.28)" "%OUT%run-dx.log" >nul || ( echo [ERROR] DX impact anchor & exit /b 1 )
findstr /c:"game-m1: traffic cars=8 lamps=401 ticks=240 odometer=128.53 arrived=0" "%OUT%run-dx.log" >nul || ( echo [ERROR] DX odometer anchor & exit /b 1 )
findstr /c:"screenshot written" "%OUT%run-dx.log" >nul || ( echo [ERROR] DX screenshot & exit /b 1 )

echo [seg3] traffic baseline VULKAN
"%GVK%" --city "%ROOT%\example\cg2-m0" --traffic 1 --cam free --cam-at -9.5,-24.1 --dist 40 --pitch -20 --yaw 0 --seed 1 --screenshot "%OUT%game-street-VULKAN.png" --screenshot-frame 8 --frames 16 --steps 240 --size 960x540 > "%OUT%run-vk.log" 2>&1
findstr /c:"game-m1: traffic cars=8 lamps=401 ticks=240 odometer=128.53 arrived=0" "%OUT%run-vk.log" >nul || ( echo [ERROR] VK odometer anchor & exit /b 1 )
findstr /c:"screenshot written" "%OUT%run-vk.log" >nul || ( echo [ERROR] VK screenshot & exit /b 1 )

echo [seg4] determinism rerun (DX12, byte-identical PNG)
"%GDX%" --city "%ROOT%\example\cg2-m0" --traffic 1 --cam free --cam-at -9.5,-24.1 --dist 40 --pitch -20 --yaw 0 --seed 1 --screenshot "%OUT%dup-dx.png" --screenshot-frame 8 --frames 16 --steps 240 --size 960x540 > "%OUT%run-dup.log" 2>&1
fc /b >nul "%OUT%game-street.png" "%OUT%dup-dx.png" || ( echo [ERROR] PNG determinism drift & exit /b 1 )

echo [seg5] negative (--traffic bad value = exit 2)
"%GDX%" --traffic 5 > "%OUT%negative.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative exit not 2 & exit /b 1 )

echo [seg6] M-GAME0 no-traffic regression (default bytes untouched)
"%GDX%" --city "%ROOT%\example\cg2-m0" --screenshot "%OUT%m0-chk-dx.png" --screenshot-frame 8 --frames 16 --steps 240 --size 960x540 --seed 1 > "%OUT%run-m0.log" 2>&1
findstr /c:"game-m0: frames=16 steps=240 objects=160 boxes=150 hits=1" "%OUT%run-m0.log" >nul || ( echo [ERROR] M-GAME0 no-traffic anchor & exit /b 1 )
del /q "%OUT%dup-dx.png" "%OUT%m0-chk-dx.png" 2>nul
echo game-m1 ALL GREEN
endlocal

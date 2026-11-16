@echo off
rem M-GAME0 min-playable gate (standalone game.exe: city drive + impact feedback).
rem Runs game selftest / phys-tools city-impact anchors / dual-backend city drive
rem with impact + screenshot baselines / determinism / negative.
setlocal
set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "GDX=%ROOT%\build\DX12\game.exe"
set "GVK=%ROOT%\build\VULKAN\game.exe"
set "PDX=%ROOT%\build\DX12\phys-tools.exe"
set "PVK=%ROOT%\build\VULKAN\phys-tools.exe"

echo [seg1] game selftest
"%GDX%" --selftest > "%OUT%selftest.log" 2>&1
findstr /c:"game-m0 selftest: PASS" "%OUT%selftest.log" >nul || (
    echo [ERROR] game selftest not PASS
    exit /b 1
)

echo [seg2] phys-tools city-impact anchors (dual backend)
"%PDX%" selftest > "%OUT%phys-dx.log" 2>&1
findstr /c:"phys-game0 city-impact: PASS" "%OUT%phys-dx.log" >nul || (
    echo [ERROR] DX city-impact anchor
    exit /b 1
)
"%PVK%" selftest > "%OUT%phys-vk.log" 2>&1
findstr /c:"phys-game0 city-impact: PASS" "%OUT%phys-vk.log" >nul || (
    echo [ERROR] VK city-impact anchor
    exit /b 1
)

echo [seg3] city drive DX12 baseline
"%GDX%" --city "%ROOT%\example\cg2-m0" --screenshot "%OUT%game-street.png" --screenshot-frame 8 --frames 16 --steps 240 --size 960x540 --seed 1 > "%OUT%run-dx.log" 2>&1
findstr /c:"physics ready: 150 city collision boxes, chassis spawn (0.0, -35.0)" "%OUT%run-dx.log" >nul || ( echo [ERROR] DX spawn anchor & exit /b 1 )
findstr /c:"GAME_IMPACT frame_hits=1 owner=113 strength=4.7787 impulse=7167.9883 pos=(87.16,4.50,3.28)" "%OUT%run-dx.log" >nul || ( echo [ERROR] DX impact anchor & exit /b 1 )
findstr /c:"game-m0: frames=16 steps=240 objects=160 boxes=150 hits=1 speed=0.027 odometer_pos=(-0.00,0.86,-30.33) shake=0.0000" "%OUT%run-dx.log" >nul || ( echo [ERROR] DX odometer anchor & exit /b 1 )
findstr /c:"screenshot written" "%OUT%run-dx.log" >nul || ( echo [ERROR] DX screenshot & exit /b 1 )

echo [seg4] city drive VULKAN baseline
"%GVK%" --city "%ROOT%\example\cg2-m0" --screenshot "%OUT%game-street-VULKAN.png" --screenshot-frame 8 --frames 16 --steps 240 --size 960x540 --seed 1 > "%OUT%run-vk.log" 2>&1
findstr /c:"GAME_IMPACT frame_hits=1 owner=113 strength=4.7787 impulse=7167.9883 pos=(87.16,4.50,3.28)" "%OUT%run-vk.log" >nul || ( echo [ERROR] VK impact anchor & exit /b 1 )
findstr /c:"game-m0: frames=16 steps=240 objects=160 boxes=150 hits=1 speed=0.027 odometer_pos=(-0.00,0.86,-30.33) shake=0.0000" "%OUT%run-vk.log" >nul || ( echo [ERROR] VK odometer anchor & exit /b 1 )
findstr /c:"screenshot written" "%OUT%run-vk.log" >nul || ( echo [ERROR] VK screenshot & exit /b 1 )

echo [seg5] determinism rerun (DX12, byte-identical PNG)
"%GDX%" --city "%ROOT%\example\cg2-m0" --screenshot "%OUT%dup-dx.png" --screenshot-frame 8 --frames 16 --steps 240 --size 960x540 --seed 1 > "%OUT%run-dup.log" 2>&1
fc /b >nul "%OUT%game-street.png" "%OUT%dup-dx.png" || ( echo [ERROR] PNG determinism drift & exit /b 1 )

echo [seg6] negative (scene not found = exit 2)
"%GDX%" --city "%OUT%nonexistent-dir" > "%OUT%negative.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative exit not 2 & exit /b 1 )

del /q "%OUT%dup-dx.png" 2>nul
echo game-m0 ALL GREEN
endlocal

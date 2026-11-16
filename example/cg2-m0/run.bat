@echo off
rem M-CG2 citygen engine delivery gate (tool + viewer consumption).
rem Runs selftest / drive determinism / json structure / frozen zero-drift /
rem viewer dual-backend image anchors / usage / negative.
setlocal
set "ROOT=%~dp0..\.."
set "PY=%ROOT%\scripts\citygen2engine.py"
set "OUT=%~dp0"
set "SRC=%ROOT%\example\cg1-m0"
set "VDX=%ROOT%\build\DX12\viewer.exe"
set "VVK=%ROOT%\build\VULKAN\viewer.exe"

echo [seg1] selftest 59 assertions
python "%PY%" selftest > "%OUT%selftest.log" 2>&1
findstr /c:"citygen2engine selftest: PASS (59 assertions)" "%OUT%selftest.log" >nul || (
    echo [ERROR] selftest not PASS 59
    exit /b 1
)

echo [seg2] drive determinism rerun
python "%PY%" drive --src "%SRC%" --out "%OUT%dup" > "%OUT%drive-dup.log" 2>&1
findstr /c:"citygen2engine drive OK" "%OUT%drive-dup.log" >nul || (
    echo [ERROR] drive rerun failed
    exit /b 1
)
fc /b >nul "%OUT%city.scene.engine" "%OUT%dup\city.scene.engine" || ( echo [ERROR] scene engine differs & exit /b 1 )
fc /b >nul "%OUT%roadnet.json" "%OUT%dup\roadnet.json" || ( echo [ERROR] roadnet differs & exit /b 1 )
fc /b >nul "%OUT%city_delivery.png" "%OUT%dup\city_delivery.png" || ( echo [ERROR] evidence png differs & exit /b 1 )

echo [seg3] json structure + asset closure
python -c "import json,pathlib; d=json.load(open(r'%OUT%city.scene.engine',encoding='utf-8')); e=d['entities']; assert d['version']==1 and len(e)==159; assert sum(1 for x in e if x['name'].startswith('car_'))==8; assert all('/' not in x['components']['Model']['mesh'] for x in e if 'Model' in x['components']); print('json OK')" > "%OUT%json-check.log" 2>&1
findstr /c:"json OK" "%OUT%json-check.log" >nul || (
    echo [ERROR] engine scene json structure
    exit /b 1
)
if not exist "%OUT%canton_tower.glb" ( echo [ERROR] tower glb missing & exit /b 1 )
if not exist "%ROOT%\assets\models\city_box_tier1.glb" ( echo [ERROR] tier1 glb missing & exit /b 1 )
if not exist "%OUT%ROADNET-ADAPT.txt" ( echo [ERROR] roadnet adapter note missing & exit /b 1 )

echo [seg4] frozen cg1 zero drift
fc /b >nul "%SRC%\city.scene" "%ROOT%\example\cg1-m0\city.scene" || ( echo [ERROR] frozen scene drift & exit /b 1 )
findstr /c:"source nodes=375 edges=417" "%OUT%ROADNET-ADAPT.txt" >nul || (
    echo [ERROR] source roadnet anchor missing
    exit /b 1
)

echo [seg5] viewer street anchors
"%VDX%" --traffic-city "%OUT%." --traffic-steps 75 --no-hud --screenshot "%OUT%verify-dx.png" --frames 8 --size 960x540 --yaw 28 --pitch -10 --dist 300 > "%OUT%viewer-dx.log" 2>&1
findstr /c:"traffic-city: 8 cars registered" "%OUT%viewer-dx.log" >nul || ( echo [ERROR] DX cars anchor & exit /b 1 )
findstr /c:"screenshot written" "%OUT%viewer-dx.log" >nul || ( echo [ERROR] DX screenshot & exit /b 1 )
"%VVK%" --traffic-city "%OUT%." --traffic-steps 75 --no-hud --screenshot "%OUT%verify-vk.png" --frames 8 --size 960x540 --yaw 28 --pitch -10 --dist 300 > "%OUT%viewer-vk.log" 2>&1
findstr /c:"traffic-city: 8 cars registered" "%OUT%viewer-vk.log" >nul || ( echo [ERROR] VK cars anchor & exit /b 1 )
findstr /c:"screenshot written" "%OUT%viewer-vk.log" >nul || ( echo [ERROR] VK screenshot & exit /b 1 )

echo [seg6] usage + negative
python "%PY%" > "%OUT%usage.log" 2>&1
if not errorlevel 1 ( echo [ERROR] usage exit not 2 & exit /b 1 )
python "%PY%" drive --src nonexistent-dir --out "%OUT%dup2" > "%OUT%negative.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative exit not 2 & exit /b 1 )

rmdir /s /q "%OUT%dup" 2>nul
rmdir /s /q "%OUT%dup2" 2>nul
del /q "%OUT%verify-dx.png" "%OUT%verify-vk.png" 2>nul
echo cg2-m0 ALL GREEN
endlocal

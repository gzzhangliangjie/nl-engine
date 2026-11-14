@echo off
rem ---- w1-m0 run.bat : city2engine M-W1.0 acceptance (city-tools products -> engine assets)
rem ---- segments: 1 driver selftest / 2 drive / 3 product anchors / 4 scene JSON validity /
rem ---- 5 same-path determinism / 6 usage / 7 negative
setlocal enabledelayedexpansion
set D=%~dp0
set PY=python
set DRIVER=E:\UE_STUDY_2026\nl-engine-worldgen\scripts\city2engine.py
set NT=E:\UE_STUDY_2026\nl-engine-worldgen\build\DX12\nanite-tools.exe
set SRC=E:\UE_STUDY_2026\nl-engine-worldgen\example\w0-m3

echo === 1 driver selftest ===
%PY% "%DRIVER%" selftest > "%D%selftest.log" 2>&1
if errorlevel 1 (
    echo [ERROR] driver selftest FAILED
    type "%D%selftest.log"
    exit /b 1
)
findstr /c:"city2engine selftest: PASS" "%D%selftest.log" >nul || (
    echo [ERROR] missing selftest PASS anchor
    exit /b 1
)
echo driver selftest PASS OK

echo === 2 drive seed=12345 size=6 ===
%PY% "%DRIVER%" drive --src "%SRC%" --seed 12345 --size 6 --nanite "%NT%" > "%D%drive.log" 2>&1
if errorlevel 1 (
    echo [ERROR] drive FAILED
    type "%D%drive.log"
    exit /b 1
)
findstr /c:"city2engine drive OK: blocks=36 glb=36 nmesh_suites=36 dag=36 scene_refs=36" "%D%drive.log" >nul || (
    echo [ERROR] missing drive stats anchor
    exit /b 1
)
echo drive OK (36 blocks -> glb + nanite suite + city.scene.engine)

echo === 3 product anchors ===
%PY% "%D%w1_check.py" anchors
if errorlevel 1 ( echo [ERROR] anchors check FAILED & exit /b 1 )

echo === 4 scene JSON validity ===
%PY% "%D%w1_check.py" json
if errorlevel 1 ( echo [ERROR] json check FAILED & exit /b 1 )

echo === 5 same-path determinism ===
%PY% "%D%w1_check.py" hash > "%D%h1.txt" 2>&1
%PY% "%DRIVER%" drive --src "%SRC%" --seed 12345 --size 6 --nanite "%NT%" > "%D%drive2.log" 2>&1
if errorlevel 1 ( echo [ERROR] second drive FAILED & exit /b 1 )
%PY% "%D%w1_check.py" hash > "%D%h2.txt" 2>&1
fc /b "%D%h1.txt" "%D%h2.txt" >nul || (
    echo [ERROR] same-path double-run hashes differ
    exit /b 1
)
del /q "%D%h1.txt" "%D%h2.txt" 2>nul
echo same-path double-run aggregate hash identical OK

echo === 6 usage exit 2 ===
%PY% "%DRIVER%" > "%D%usage.log" 2>&1
if not errorlevel 1 ( echo [ERROR] usage should exit 2 & exit /b 1 )
findstr /c:"city2engine - nl-engine worldgen tools" "%D%usage.log" >nul || (
    echo [ERROR] usage banner missing
    exit /b 1
)
echo usage exit 2 OK

echo === 7 negative: --size 0 ===
%PY% "%DRIVER%" drive --src "%SRC%" --seed 12345 --size 0 --nanite "%NT%" > "%D%negative.log" 2>&1
if not errorlevel 1 ( echo [ERROR] --size 0 should exit 2 & exit /b 1 )
findstr /c:"invalid arguments: --size out of range" "%D%negative.log" >nul || (
    echo [ERROR] negative message missing
    exit /b 1
)
echo negative exit 2 OK

echo ALL GREEN
exit /b 0

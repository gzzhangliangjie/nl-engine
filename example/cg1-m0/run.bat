@echo off
rem M-CG1 citygen landmark gate (max hero glb + OSM base, ZJX fixture anchors).
rem Runs selftest(9 groups) / export with whitelist / landmark anchors /
rem determinism / usage / negative. All anchors are substring finds.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\citygen.exe"
set "OUT=%~dp0"
set "IN=%ROOT%\modules\citygen\test\zjx-sample.json"
set "BLD=%ROOT%\modules\citygen\test\zjx-buildings-sample.json"
set "WL=%ROOT%\assets\citygen\landmark_whitelist.json"
set "BBOX=23.108,113.316,23.130,113.342"

echo [seg1] selftest 9 groups
"%EXE%" selftest > "%OUT%selftest.log" 2>&1
findstr /c:"citygen selftest: PASS (9 groups, mini overpass sample)" "%OUT%selftest.log" >nul || (
    echo [ERROR] selftest not PASS 9 groups
    exit /b 1
)

echo [seg2] export with landmark whitelist
"%EXE%" export --input "%IN%" --buildings "%BLD%" --bbox "%BBOX%" --landmarks "%WL%" --out "%OUT%." > "%OUT%export.log" 2>&1
findstr /c:"buildings=150 landmarks=1 suppressed=0 outside=1" "%OUT%export.log" >nul || (
    echo [ERROR] landmark stats anchor missing
    exit /b 1
)
findstr /c:"bldPx=263673" "%OUT%export.log" >nul || (
    echo [ERROR] base top-view content anchor missing
    exit /b 1
)
findstr /c:"lmPx=256" "%OUT%export.log" >nul || (
    echo [ERROR] landmark gold pixel anchor missing
    exit /b 1
)

echo [seg3] landmark entity in city.scene
findstr /c:"canton_tower.glb" "%OUT%city.scene" >nul || (
    echo [ERROR] city.scene missing landmark mesh
    exit /b 1
)
findstr /c:"lm_0" "%OUT%city.scene" >nul || (
    echo [ERROR] city.scene missing landmark entity
    exit /b 1
)
if not exist "%OUT%landmark_whitelist.json" ( echo [ERROR] whitelist copy missing & exit /b 1 )
if not exist "%OUT%ODbL-attribution.txt" ( echo [ERROR] ODbL attribution missing & exit /b 1 )

echo [seg4] determinism rerun byte compare
"%EXE%" export --input "%IN%" --buildings "%BLD%" --bbox "%BBOX%" --landmarks "%WL%" --out "%OUT%dup" > nul 2>&1
fc /b >nul "%OUT%city.scene" "%OUT%dup\city.scene" || ( echo [ERROR] city.scene differs & exit /b 1 )
fc /b >nul "%OUT%top.png" "%OUT%dup\top.png" || ( echo [ERROR] top.png differs & exit /b 1 )

echo [seg5] usage exit 2
"%EXE%" > "%OUT%usage.log" 2>&1
if not errorlevel 1 ( echo [ERROR] usage exit not 2 & exit /b 1 )
findstr /c:"--landmarks" "%OUT%usage.log" >nul || (
    echo [ERROR] usage missing landmarks flag
    exit /b 1
)

echo [seg6] negative exit 2
"%EXE%" stats --input nonexistent.json > "%OUT%negative.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative exit not 2 & exit /b 1 )
findstr /c:"citygen: cannot read 'nonexistent.json'" "%OUT%negative.log" >nul || (
    echo [ERROR] negative banner missing
    exit /b 1
)

rmdir /s /q "%OUT%dup" 2>nul
echo cg1-m0 ALL GREEN
endlocal

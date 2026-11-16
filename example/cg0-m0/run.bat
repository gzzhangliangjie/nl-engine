@echo off
rem M-CG0 citygen tool gate (OSM real-map input, ZJX fixture anchors).
rem Runs selftest(9 groups) / convert / export / determinism / usage / negative.
rem All anchors are substring finds (findstr).
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\citygen.exe"
set "OUT=%~dp0"
set "IN=%ROOT%\modules\citygen\test\zjx-sample.json"
set "BLD=%ROOT%\modules\citygen\test\zjx-buildings-sample.json"
set "BBOX=23.108,113.316,23.130,113.342"

echo [seg1] selftest 9 groups
"%EXE%" selftest > "%OUT%selftest.log" 2>&1
findstr /c:"citygen selftest: PASS (9 groups, mini overpass sample)" "%OUT%selftest.log" >nul || (
    echo [ERROR] selftest not PASS 9 groups
    exit /b 1
)

echo [seg2] convert fixture -> roadnet.json
"%EXE%" convert --input "%IN%" --bbox "%BBOX%" --out "%OUT%roadnet.json" > "%OUT%convert.log" 2>&1
findstr /c:"nodes=375 edges=417" "%OUT%convert.log" >nul || (
    echo [ERROR] convert topology anchor missing 375 nodes 417 edges
    exit /b 1
)

echo [seg3] export four-piece bundle + anchors
"%EXE%" export --input "%IN%" --buildings "%BLD%" --bbox "%BBOX%" --out "%OUT%." > "%OUT%export.log" 2>&1
findstr /c:"buildings=150" "%OUT%export.log" >nul || (
    echo [ERROR] export building count anchor missing 150
    exit /b 1
)
findstr /c:"bldPx=263136" "%OUT%export.log" >nul || (
    echo [ERROR] top-view content anchor missing bldPx 263136
    exit /b 1
)
if not exist "%OUT%ODbL-attribution.txt" ( echo [ERROR] ODbL attribution missing & exit /b 1 )
findstr /c:"\"nextID\": 151" "%OUT%city.scene" >nul || (
    echo [ERROR] city.scene missing nextID 151
    exit /b 1
)

echo [seg4] determinism (convert+export rerun into dup, byte compare)
"%EXE%" convert --input "%IN%" --bbox "%BBOX%" --out "%OUT%dup-roadnet.json" > nul 2>&1
fc /b >nul "%OUT%roadnet.json" "%OUT%dup-roadnet.json" || ( echo [ERROR] roadnet.json differs & exit /b 1 )
"%EXE%" export --input "%IN%" --buildings "%BLD%" --bbox "%BBOX%" --out "%OUT%dup" > nul 2>&1
fc /b >nul "%OUT%city.scene" "%OUT%dup\city.scene" || ( echo [ERROR] city.scene differs & exit /b 1 )
fc /b >nul "%OUT%top.png" "%OUT%dup\top.png" || ( echo [ERROR] top.png differs & exit /b 1 )

echo [seg5] usage exit 2
"%EXE%" > "%OUT%usage.log" 2>&1
if not errorlevel 1 ( echo [ERROR] usage exit not 2 & exit /b 1 )
findstr /c:"citygen - nl-engine city generation (OSM input, M-CG0)" "%OUT%usage.log" >nul || (
    echo [ERROR] usage banner missing & exit /b 1
)

echo [seg6] negative exit 2
"%EXE%" stats --input nonexistent.json > "%OUT%negative.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative exit not 2 & exit /b 1 )
findstr /c:"citygen: cannot read 'nonexistent.json'" "%OUT%negative.log" >nul || (
    echo [ERROR] negative banner missing & exit /b 1
)

del /q "%OUT%dup-roadnet.json" 2>nul
rmdir /s /q "%OUT%dup" 2>nul
echo cg0-m0 ALL GREEN
endlocal

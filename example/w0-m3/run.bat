@echo off
rem ---- w0-m3 run.bat : city-tools M-W0.3 acceptance (density + trees + stats anchors)
rem ---- segments: 1 selftest / 2 generate+stats / 3 roadnet+scene+obj anchors /
rem ---- 4 same-seed byte-identical / 5 different-seed differs / 6 usage / 7 negative
setlocal enabledelayedexpansion
set D=%~dp0
set EXE=%1
if "%EXE%"=="" set EXE=E:\UE_STUDY_2026\nl-engine-worldgen\build\DX12\city-tools.exe

echo === 1 selftest ===
"%EXE%" selftest > "%D%selftest.log" 2>&1
if errorlevel 1 (
    echo [ERROR] selftest FAILED - see selftest.log
    exit /b 1
)
findstr /c:"citygen selftest: PASS" "%D%selftest.log" >nul || (
    echo [ERROR] missing selftest PASS anchor
    exit /b 1
)
echo selftest PASS OK

echo === 2 generate seed=12345 size=6 ===
"%EXE%" generate --seed 12345 --size 6 --out "%D%." --stats > "%D%generate.log" 2>&1
if errorlevel 1 (
    echo [ERROR] generate FAILED
    exit /b 1
)
findstr /c:"stats: blocks=36 groundTiles=36 roadSegments=72 crossroads=49 roadnetNodes=49 roadnetEdges=168 buildings=144" "%D%generate.log" >nul || (
    echo [ERROR] missing base stats anchors
    exit /b 1
)
findstr /c:"forestCells=21 lowCells=5 mediumCells=6 highCells=4 noiseSeed=2109909543" "%D%generate.log" >nul || (
    echo [ERROR] missing density tier anchors
    exit /b 1
)
findstr /c:"trees=188" "%D%generate.log" >nul || (
    echo [ERROR] missing trees anchor
    exit /b 1
)
echo generate stats anchors OK (36 blocks / 144 buildings / tiers 21-5-6-4 / 188 trees)

echo === 3 product anchors ===
if not exist "%D%roadnet.json" ( echo [ERROR] roadnet.json missing & exit /b 1 )
if not exist "%D%city.scene" ( echo [ERROR] city.scene missing & exit /b 1 )
if not exist "%D%blocks\block_00.obj" ( echo [ERROR] block_00.obj missing & exit /b 1 )
for /f %%a in ('dir /b "%D%blocks\*.obj" ^| find /c /v ""') do set NOBJ=%%a
if not "%NOBJ%"=="36" (
    echo [ERROR] OBJ count %NOBJ% != 36
    exit /b 1
)
pushd "%D%"
for /f "tokens=3 delims=: " %%a in ('find /c "intersection" "roadnet.json"') do set KINDS=%%a
popd
if not "%KINDS%"=="49" (
    echo [ERROR] intersection nodes %KINDS% != 49
    exit /b 1
)
echo product anchors OK (36 OBJ / 49 nodes / scene + roadnet present)

echo === 4 same-seed determinism ===
"%EXE%" generate --seed 12345 --size 6 --out "%D%dup" > "%D%dup.log" 2>&1
if errorlevel 1 ( echo [ERROR] dup generate FAILED & exit /b 1 )
fc /b "%D%city_top.png" "%D%dup\city_top.png" >nul || (
    echo [ERROR] same seed PNG bytes differ
    exit /b 1
)
fc /b "%D%roadnet.json" "%D%dup\roadnet.json" >nul || (
    echo [ERROR] same seed roadnet.json bytes differ
    exit /b 1
)
fc /b "%D%city.scene" "%D%dup\city.scene" >nul || (
    echo [ERROR] same seed city.scene bytes differ
    exit /b 1
)
fc /b "%D%blocks\block_07.obj" "%D%dup\blocks\block_07.obj" >nul || (
    echo [ERROR] same seed OBJ bytes differ
    exit /b 1
)
del /q "%D%dup\city_top.png" "%D%dup\roadnet.json" "%D%dup\city.scene" "%D%dup\generate.log" 2>nul
for %%f in ("%D%dup\blocks\*.obj") do del /q "%%f" 2>nul
rmdir "%D%dup\blocks" 2>nul
rmdir "%D%dup" 2>nul
echo same-seed PNG + roadnet + scene + OBJ byte-identical OK

echo === 5 different seed must differ ===
"%EXE%" generate --seed 12346 --size 6 --out "%D%other" --stats > "%D%other.log" 2>&1
if errorlevel 1 ( echo [ERROR] other-seed generate FAILED & exit /b 1 )
findstr /c:"forestCells=19 lowCells=4 mediumCells=4 highCells=9" "%D%other.log" >nul || (
    echo [ERROR] different-seed density anchors missing
    exit /b 1
)
fc /b "%D%city_top.png" "%D%other\city_top.png" >nul && (
    echo [ERROR] different seed PNG identical
    exit /b 1
)
fc /b "%D%blocks\block_07.obj" "%D%other\blocks\block_07.obj" >nul && (
    echo [ERROR] different seed OBJ identical
    exit /b 1
)
fc /b "%D%city.scene" "%D%other\city.scene" >nul || (
    echo [ERROR] city.scene must stay seed-agnostic
    exit /b 1
)
del /q "%D%other\city_top.png" "%D%other\roadnet.json" "%D%other\city.scene" "%D%other\generate.log" "%D%other.log" 2>nul
for %%f in ("%D%other\blocks\*.obj") do del /q "%%f" 2>nul
rmdir "%D%other\blocks" 2>nul
rmdir "%D%other" 2>nul
echo different-seed PNG + OBJ differ, density tiers differ, scene seed-agnostic OK

echo === 6 usage exit 2 ===
"%EXE%" > "%D%usage.log" 2>&1
if not errorlevel 1 ( echo [ERROR] usage should exit 2 & exit /b 1 )
findstr /c:"city-tools - nl-engine worldgen tools" "%D%usage.log" >nul || (
    echo [ERROR] usage banner missing
    exit /b 1
)
echo usage exit 2 OK

echo === 7 negative: --size 0 ===
"%EXE%" generate --size 0 --out "%D%bad" > "%D%negative.log" 2>&1
if not errorlevel 1 ( echo [ERROR] --size 0 should exit 2 & exit /b 1 )
findstr /c:"invalid arguments: --size out of range" "%D%negative.log" >nul || (
    echo [ERROR] negative message missing
    exit /b 1
)
echo negative exit 2 OK

echo ALL GREEN
exit /b 0

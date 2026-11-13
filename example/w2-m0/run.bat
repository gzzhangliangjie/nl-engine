@echo off
rem ---- w2-m0 run.bat : M-W2.0 building variant pool acceptance (city-tools)
rem ---- segments: 1 selftest / 2 generate+stats+variants / 3 product anchors /
rem ---- 4 same-seed determinism / 5 other-seed variants / 6 usage / 7 negative
rem ---- All output ASCII. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\city-tools.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] city-tools not built: build\dx12\city-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === 1 selftest ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] citygen selftest FAILED
    exit /b 1
)
type "%D%selftest.log"
findstr /c:"citygen selftest: PASS (88 assertions)" "%D%selftest.log" >nul || (
    echo [ERROR] missing selftest PASS (88) anchor
    exit /b 1
)
findstr /c:"variant: all buildings inside their tier pool (M-W2.0)" "%D%selftest.log" >nul || (
    echo [ERROR] missing variant pool anchor
    exit /b 1
)
echo selftest PASS (88 assertions) OK

echo === 2 generate seed=12345 size=6 ===
"%EXE%" generate --seed 12345 --size 6 --out "%D%." --stats > "%D%generate.log" 2>&1
if errorlevel 1 (
    type "%D%generate.log"
    echo [ERROR] generate FAILED
    exit /b 1
)
type "%D%generate.log"
findstr /c:"stats: blocks=36 groundTiles=36 roadSegments=72 crossroads=49 roadnetNodes=49 roadnetEdges=168 buildings=144" "%D%generate.log" >nul || (
    echo [ERROR] missing stats anchors
    exit /b 1
)
findstr /c:"forestCells=21 lowCells=5 mediumCells=6 highCells=4 noiseSeed=2109909543" "%D%generate.log" >nul || (
    echo [ERROR] missing density tier anchors
    exit /b 1
)
rem M-W2.0 variant distribution anchor (FNV-1a pure function -> seed-deterministic)
findstr /c:"variants: single=48 setback=54 podium=8 lshape=34 (M-W2.0 fnv1a pool)" "%D%generate.log" >nul || (
    echo [ERROR] missing variants distribution anchor
    exit /b 1
)
echo generate stats + variants anchors OK (48/54/8/34 = single/setback/podium/lshape)

echo === 3 product anchors ===
if not exist "%D%roadnet.json" ( echo [ERROR] roadnet.json missing & exit /b 1 )
if not exist "%D%city.scene" ( echo [ERROR] city.scene missing & exit /b 1 )
if not exist "%D%blocks\block_00.obj" ( echo [ERROR] block_00.obj missing & exit /b 1 )
if not exist "%D%blocks\city.mtl" ( echo [ERROR] city.mtl missing & exit /b 1 )
if not exist "%D%textures\tier1.jpg" ( echo [ERROR] textures\tier1.jpg missing & exit /b 1 )
for /f %%a in ('dir /b "%D%blocks\*.obj" ^| find /c /v ""') do set NOBJ=%%a
if not "%NOBJ%"=="36" (
    echo [ERROR] OBJ count %NOBJ% != 36
    exit /b 1
)
echo product anchors OK (36 OBJ + city.mtl + textures + scene + roadnet)

echo === 4 same-seed determinism ===
"%EXE%" generate --seed 12345 --size 6 --out "%D%dup" > "%D%dup.log" 2>&1
if errorlevel 1 ( echo [ERROR] dup generate FAILED & exit /b 1 )
fc /b "%D%city_top.png" "%D%dup\city_top.png" >nul || (
    echo [ERROR] same seed PNG bytes differ
    exit /b 1
)
fc /b "%D%city.scene" "%D%dup\city.scene" >nul || (
    echo [ERROR] same seed city.scene bytes differ
    exit /b 1
)
fc /b "%D%blocks\block_07.obj" "%D%dup\blocks\block_07.obj" >nul || (
    echo [ERROR] same seed OBJ bytes differ (variants included)
    exit /b 1
)
echo same-seed PNG + scene + OBJ byte-identical OK

echo === 5 other-seed variants (seed-sensitive assignment) ===
"%EXE%" generate --seed 12346 --size 6 --out "%D%other" --stats > "%D%other.log" 2>&1
if errorlevel 1 ( echo [ERROR] other-seed generate FAILED & exit /b 1 )
findstr /c:"variants: single=49 setback=54 podium=10 lshape=31 (M-W2.0 fnv1a pool)" "%D%other.log" >nul || (
    echo [ERROR] missing other-seed variants anchor (49/54/10/31)
    exit /b 1
)
fc /b "%D%blocks\block_07.obj" "%D%other\blocks\block_07.obj" >nul && (
    echo [ERROR] different seed OBJ identical - variant assignment lost
    exit /b 1
)
echo different-seed variants differ (48/54/8/34 vs 49/54/10/31) OK

echo === 6 usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
findstr /c:"city-tools - nl-engine worldgen tools" "%D%usage.log" >nul || (
    echo [ERROR] usage banner missing
    exit /b 1
)

echo === 7 negative: --size 0 (exit 2 expected) ===
"%EXE%" generate --size 0 --out "%D%bad" > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --size should exit 2
    exit /b 1
)
findstr /c:"invalid arguments: --size out of range" "%D%negative.log" >nul || (
    echo [ERROR] negative message missing
    exit /b 1
)

echo === cleanup temp dirs ===
del /q "%D%dup\blocks\*.*" 2>nul
rmdir "%D%dup\blocks" 2>nul
del /q "%D%dup\textures\*.*" 2>nul
rmdir "%D%dup\textures" 2>nul
del /q "%D%dup\*.*" 2>nul
rmdir "%D%dup" 2>nul
del /q "%D%other\blocks\*.*" 2>nul
rmdir "%D%other\blocks" 2>nul
del /q "%D%other\textures\*.*" 2>nul
rmdir "%D%other\textures" 2>nul
del /q "%D%other\*.*" 2>nul
rmdir "%D%other" 2>nul
del /q "%D%bad\*.*" 2>nul
rmdir "%D%bad" 2>nul

echo.
echo w2-m0 run.bat ALL GREEN
endlocal

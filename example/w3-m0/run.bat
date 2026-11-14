@echo off
rem M-W3.0 street-asset channel tool gate (city-tools M-W3.0 anchors).
rem Runs selftest(97) / generate with street distribution line / determinism /
rem other-seed / usage / negative. All anchors are substring finds (findstr).
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\city-tools.exe"
set "OUT=%~dp0"

echo [seg1] selftest --verbose
"%EXE%" selftest --verbose > "%OUT%selftest.log" 2>&1
findstr /c:"citygen selftest: PASS (97 assertions)" "%OUT%selftest.log" >nul || (
    echo [ERROR] selftest not PASS(97)
    exit /b 1
)

echo [seg2] generate seed 12345 size 6 + street anchors
"%EXE%" generate --seed 12345 --size 6 --out "%OUT%." --stats > "%OUT%generate.log" 2>&1
findstr /c:"street: cars=42 lights=49 treeVariants=14,13,16,18,17,16,14,13,16,18,17,16" "%OUT%generate.log" >nul || (
    echo [ERROR] street distribution anchor missing
    exit /b 1
)
findstr /c:"variants: single=48 setback=54 podium=8 lshape=34" "%OUT%generate.log" >nul || (
    echo [ERROR] M-W2.0 variants anchor missing
    exit /b 1
)
findstr /c:"\"nextID\": 316" "%OUT%city.scene" >nul || (
    echo [ERROR] city.scene missing nextID 316 (blocks+trees+cars+lights)
    exit /b 1
)

echo [seg3] product files
if not exist "%OUT%roadnet.json" ( echo [ERROR] roadnet.json missing & exit /b 1 )
if not exist "%OUT%blocks\city.mtl" ( echo [ERROR] city.mtl missing & exit /b 1 )
if not exist "%OUT%textures\tier1.jpg" ( echo [ERROR] textures missing & exit /b 1 )
for /f %%c in ('dir /b "%OUT%blocks\*.obj" 2^>nul ^| find /c /v ""') do (
    if not "%%c"=="36" ( echo [ERROR] OBJ count %%c != 36 & exit /b 1 )
)

echo [seg4] same-seed determinism
"%EXE%" generate --seed 12345 --size 6 --out "%OUT%dup" > "%OUT%dup.log" 2>&1
fc /b >nul "%OUT%city.scene" "%OUT%dup\city.scene" || ( echo [ERROR] city.scene differs & exit /b 1 )
fc /b >nul "%OUT%blocks\block_07.obj" "%OUT%dup\blocks\block_07.obj" || ( echo [ERROR] block OBJ differs & exit /b 1 )

echo [seg5] other-seed street anchors
"%EXE%" generate --seed 12346 --size 6 --out "%OUT%other" --stats > "%OUT%other.log" 2>&1
findstr /c:"street: cars=42 lights=49 treeVariants=14,14,14,14,14,14,14,14,14,14,14,14" "%OUT%other.log" >nul || (
    echo [ERROR] seed 12346 street anchor missing
    exit /b 1
)

echo [seg6] usage exit 2
"%EXE%" > "%OUT%usage.log" 2>&1
if not errorlevel 1 ( echo [ERROR] usage exit not 2 & exit /b 1 )
findstr /c:"city-tools - nl-engine worldgen tools" "%OUT%usage.log" >nul || (
    echo [ERROR] usage banner missing & exit /b 1
)

echo [seg7] negative exit 2
"%EXE%" generate --size 0 --out "%OUT%bad" > "%OUT%negative.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative exit not 2 & exit /b 1 )
findstr /c:"invalid arguments: --size out of range" "%OUT%negative.log" >nul || (
    echo [ERROR] negative banner missing & exit /b 1
)

rmdir /s /q "%OUT%dup" 2>nul
rmdir /s /q "%OUT%other" 2>nul
rmdir /s /q "%OUT%bad" 2>nul
echo w3-m0 ALL GREEN
endlocal

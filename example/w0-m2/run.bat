@echo off
rem W0 M-W0.2 acceptance: selftest + procedural buildings -> per-block OBJ
rem intermediates + city.scene (E1 World JSON) + overlay top view + determinism.
rem All output ASCII. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\city-tools.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] city-tools not built: build\dx12\city-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest --verbose ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] citygen selftest FAILED
    exit /b 1
)
type "%D%selftest.log"

echo === generate seed=12345 size=6 ===
rem "%D%." strips the trailing backslash of %~dp0 so the quoted --out arg
rem does not end with \" (which the C runtime would parse as a literal quote).
"%EXE%" generate --seed 12345 --size 6 --out "%D%." --stats > "%D%generate.log" 2>&1
if errorlevel 1 (
    type "%D%generate.log"
    echo [ERROR] generate FAILED
    exit /b 1
)
type "%D%generate.log"
for %%f in ("%D%city_top.png" "%D%roadnet.json" "%D%city.scene" "%D%blocks\block_00.obj" "%D%blocks\block_35.obj") do (
    if not exist %%f (
        echo [ERROR] missing %%f
        exit /b 1
    )
)
findstr /c:"stats: blocks=36 groundTiles=36 roadSegments=72 crossroads=49 roadnetNodes=49 roadnetEdges=168 buildings=144" "%D%generate.log" >nul || (
    echo [ERROR] missing stats anchors
    exit /b 1
)

echo === city.scene contract anchors (E1 World JSON) ===
findstr /c:"\"version\": 1" "%D%city.scene" >nul || (
    echo [ERROR] city.scene missing version field
    exit /b 1
)
findstr /c:"\"nextID\": 37" "%D%city.scene" >nul || (
    echo [ERROR] city.scene missing nextID 37
    exit /b 1
)
findstr /c:"\"systems\": []" "%D%city.scene" >nul || (
    echo [ERROR] city.scene missing systems placeholder
    exit /b 1
)
pushd "%D%"
rem find /c prints "---------- FILE: N": count lands in token 3 (dash run is
rem token 1, filename token 2). Patterns are quote-free on purpose: escaped
rem quotes inside for /f backquotes get mangled by cmd.
rem Each entity spans 4 lines: "name" (with id/flags/parent) and "mesh" occur
rem exactly once per entity, so their line counts are the entity/mesh anchors.
for /f "tokens=3 delims=: " %%a in ('find /c "name" "city.scene"') do set ENTS=%%a
for /f "tokens=3 delims=: " %%a in ('find /c "mesh" "city.scene"') do set MESH=%%a
for /f %%a in ('dir /b "blocks" ^| find /c /v ""') do set OBJN=%%a
popd
if not "%ENTS%"=="36" (
    echo [ERROR] entity lines %ENTS% != 36
    exit /b 1
)
if not "%MESH%"=="36" (
    echo [ERROR] mesh reference lines %MESH% != 36
    exit /b 1
)
if not "%OBJN%"=="36" (
    echo [ERROR] emitted OBJ files %OBJN% != 36
    exit /b 1
)
echo city.scene anchors OK (36 entities / 36 mesh refs / 36 OBJ files closed)

echo === determinism: regenerate same seed, artifacts byte-identical ===
"%EXE%" generate --seed 12345 --size 6 --out "%D%dup" > "%D%dup.log" 2>&1
if errorlevel 1 (
    echo [ERROR] regenerate FAILED
    exit /b 1
)
fc /b "%D%city.scene" "%D%dup\city.scene" >nul || (
    echo [ERROR] same seed city.scene bytes differ - determinism broken
    exit /b 1
)
fc /b "%D%blocks\block_07.obj" "%D%dup\blocks\block_07.obj" >nul || (
    echo [ERROR] same seed OBJ bytes differ - determinism broken
    exit /b 1
)
fc /b "%D%city_top.png" "%D%dup\city_top.png" >nul || (
    echo [ERROR] same seed PNG bytes differ - determinism broken
    exit /b 1
)
echo same-seed scene + OBJ + PNG byte-identical OK
del /q "%D%dup\blocks\*.*"
rmdir "%D%dup\blocks"
del /q "%D%dup\*.*"
rmdir "%D%dup"

echo === different seed must differ (tier draws live in OBJ + PNG, not scene) ===
"%EXE%" generate --seed 12346 --size 6 --out "%D%other" > "%D%other.log" 2>&1
if errorlevel 1 (
    echo [ERROR] other-seed generate FAILED
    exit /b 1
)
fc /b "%D%blocks\block_07.obj" "%D%other\blocks\block_07.obj" >nul && (
    echo [ERROR] different seed OBJ identical - tier draws lost
    exit /b 1
)
fc /b "%D%city_top.png" "%D%other\city_top.png" >nul && (
    echo [ERROR] different seed PNG identical - tier shading lost
    exit /b 1
)
fc /b "%D%city.scene" "%D%other\city.scene" >nul || (
    echo [ERROR] different seed city.scene differs - E1 index must be seed-agnostic
    exit /b 1
)
echo different-seed OBJ + PNG differ, scene stays seed-agnostic OK
del /q "%D%other\blocks\*.*"
rmdir "%D%other\blocks"
del /q "%D%other\*.*"
rmdir "%D%other"

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: --size 0 (exit 2 expected) ===
"%EXE%" generate --size 0 --out "%D%bad" > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --size should exit 2
    exit /b 1
)
type "%D%negative.log"

echo.
echo w0-m2 run.bat ALL GREEN
endlocal

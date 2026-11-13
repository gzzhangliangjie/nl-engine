@echo off
rem W0 M-W0.1 acceptance: selftest + roadnet.json contract + overlay top view
rem + determinism (same seed byte-identical, different seed must differ).
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
if not exist "%D%roadnet.json" (
    echo [ERROR] missing roadnet.json
    exit /b 1
)
if not exist "%D%city_top.png" (
    echo [ERROR] missing city_top.png
    exit /b 1
)
findstr /c:"stats: blocks=36 groundTiles=36 roadSegments=72 crossroads=49 roadnetNodes=49 roadnetEdges=168" "%D%generate.log" >nul || (
    echo [ERROR] missing stats anchors
    exit /b 1
)

echo === roadnet.json contract anchors ===
findstr /c:"\"version\": 1" "%D%roadnet.json" >nul || (
    echo [ERROR] roadnet.json missing version field
    exit /b 1
)
findstr /c:"\"units\": \"meters\"" "%D%roadnet.json" >nul || (
    echo [ERROR] roadnet.json missing units field
    exit /b 1
)
findstr /c:"\"blocks\": { \"x\": 5.5, \"z\": 4.0 }" "%D%roadnet.json" >nul || (
    echo [ERROR] roadnet.json missing blocks contract
    exit /b 1
)
findstr /c:"\"phases\": []" "%D%roadnet.json" >nul || (
    echo [ERROR] roadnet.json missing phases placeholder
    exit /b 1
)
rem find pattern is quote-free on purpose: escaped quotes inside for /f
rem backquotes get mangled by cmd; "intersection"/"lanes_per_dir" occur in the
rem JSON exactly once per node/edge, so their counts are the contract anchors.
rem pushd first so the file arg is relative (a drive-letter colon would break
rem the delims=: token split on find /c output).
pushd "%D%"
rem find /c prints "---------- ROADNET.JSON: 49": dashes are token 1,
rem the filename token 2, the count token 3.
for /f "tokens=3 delims=: " %%a in ('find /c "intersection" "roadnet.json"') do set KINDS=%%a
for /f "tokens=3 delims=: " %%a in ('find /c "lanes_per_dir" "roadnet.json"') do set LANES=%%a
popd
if not "%KINDS%"=="49" (
    echo [ERROR] intersection node count %KINDS% != 49
    exit /b 1
)
if not "%LANES%"=="168" (
    echo [ERROR] directed edge count %LANES% != 168
    exit /b 1
)
echo roadnet contract anchors OK (49 nodes / 168 directed edges)

echo === determinism: regenerate same seed, both artifacts byte-identical ===
"%EXE%" generate --seed 12345 --size 6 --out "%D%dup" > "%D%dup.log" 2>&1
if errorlevel 1 (
    echo [ERROR] regenerate FAILED
    exit /b 1
)
fc /b "%D%city_top.png" "%D%dup\city_top.png" >nul || (
    echo [ERROR] same seed PNG bytes differ - determinism broken
    exit /b 1
)
fc /b "%D%roadnet.json" "%D%dup\roadnet.json" >nul || (
    echo [ERROR] same seed roadnet.json bytes differ - determinism broken
    exit /b 1
)
echo same-seed PNG + roadnet.json byte-identical OK
del /q "%D%dup\*.*"
rmdir "%D%dup"

echo === different seed must differ ===
"%EXE%" generate --seed 12346 --size 6 --out "%D%other" > "%D%other.log" 2>&1
if errorlevel 1 (
    echo [ERROR] other-seed generate FAILED
    exit /b 1
)
fc /b "%D%roadnet.json" "%D%other\roadnet.json" >nul && (
    echo [ERROR] different seed roadnet.json identical - seed field lost
    exit /b 1
)
echo different-seed roadnet.json differs OK
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
echo w0-m1 run.bat ALL GREEN
endlocal

@echo off
rem W0 M-W0.0 acceptance: citygen selftest + blank city top view PNG + determinism.
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
if not exist "%D%city_top.png" (
    echo [ERROR] missing city_top.png
    exit /b 1
)
findstr /c:"stats: blocks=36 groundTiles=36 roadSegments=72 crossroads=49" "%D%generate.log" >nul || (
    echo [ERROR] missing stats anchors
    exit /b 1
)

echo === determinism: regenerate same seed, PNG bytes must be identical ===
"%EXE%" generate --seed 12345 --size 6 --out "%D%dup" > "%D%dup.log" 2>&1
if errorlevel 1 (
    echo [ERROR] regenerate FAILED
    exit /b 1
)
fc /b "%D%city_top.png" "%D%dup\city_top.png" >nul || (
    echo [ERROR] same seed PNG bytes differ - determinism broken
    exit /b 1
)
echo same-seed PNG byte-identical OK
rmdir /s /q "%D%dup"

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
echo w0-m0 run.bat ALL GREEN
endlocal

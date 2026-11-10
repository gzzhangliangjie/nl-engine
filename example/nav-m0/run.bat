@echo off
rem P8 M-N0 acceptance: Recast/Detour 1.6.0 bake -> save/load roundtrip -> dump
rem + top-down wireframe evidence PNG from the real bake artifact. ASCII only.
rem Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\nav-tools.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] nav-tools not built: build\dx12\nav-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest --verbose ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] nav-tools selftest FAILED
    exit /b 1
)
type "%D%selftest.log"

echo === bake HizScene.glb (cs=0.1, agent radius=0.2) x2 for determinism ===
"%EXE%" bake "%ROOT%\assets\models\HizScene.glb" "%D%hiz-a.nnav" --cell-size 0.1 --agent-radius 0.2 > "%D%bake.log" 2>&1
if errorlevel 1 (
    type "%D%bake.log"
    echo [ERROR] bake FAILED
    exit /b 1
)
type "%D%bake.log"
"%EXE%" bake "%ROOT%\assets\models\HizScene.glb" "%D%hiz-b.nnav" --cell-size 0.1 --agent-radius 0.2 >nul 2>&1
if errorlevel 1 (
    echo [ERROR] second bake FAILED
    exit /b 1
)
fc /b "%D%hiz-a.nnav" "%D%hiz-b.nnav" >nul || (echo [ERROR] determinism: artifacts differ & exit /b 1)
echo determinism : hiz-a.nnav == hiz-b.nnav (byte-identical)
del "%D%hiz-b.nnav"

echo === dump hiz-a.nnav ===
"%EXE%" dump "%D%hiz-a.nnav" > "%D%dump.log" 2>&1
if errorlevel 1 (
    type "%D%dump.log"
    echo [ERROR] dump FAILED
    exit /b 1
)
findstr /c:"polys       : 4" "%D%dump.log" >nul || (echo [ERROR] unexpected poly count & exit /b 1)
type "%D%dump.log"

echo === resave roundtrip (byte-identical) ===
"%EXE%" resave "%D%hiz-a.nnav" "%D%hiz-rt.nnav" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] resave FAILED
    exit /b 1
)
fc /b "%D%hiz-a.nnav" "%D%hiz-rt.nnav" >nul || (echo [ERROR] roundtrip: bytes differ & exit /b 1)
echo roundtrip   : resave byte-identical
del "%D%hiz-rt.nnav"

echo === wire evidence PNG (real tool output) ===
"%EXE%" wire "%D%hiz-a.nnav" "%D%nav-wire.png" --size 512 > "%D%wire.log" 2>&1
if errorlevel 1 (
    type "%D%wire.log"
    echo [ERROR] wire FAILED
    exit /b 1
)
type "%D%wire.log"
if not exist "%D%nav-wire.png" (echo [ERROR] missing nav-wire.png & exit /b 1)

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: dump garbage magic (exit 1 expected) ===
echo XXXXgarbage > "%D%garbage.nnav"
"%EXE%" dump "%D%garbage.nnav" > "%D%negative.log" 2>&1
if errorlevel 1 if not errorlevel 2 (
    echo negative exit 1 OK
) else (
    echo [ERROR] garbage dump should exit 1
    exit /b 1
)
type "%D%negative.log"
del "%D%garbage.nnav"

echo === NAV-M0 ACCEPTANCE: ALL GREEN ===
exit /b 0

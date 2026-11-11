@echo off
rem P8 M-N5 acceptance: off-mesh connection (bake-time injection + .nnav v3)
rem Q5-1 bake-time / Q5-2 v3 / Q5-3 solo only / Q5-4 flags+exclude / Q5-5 reversal.
rem ASCII only. Exit 0 = all green. Arg 1 = DX12 (default) or VULKAN.
setlocal
set "BACKEND=%~1"
if "%BACKEND%"=="" set "BACKEND=DX12"
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\%BACKEND%\nav-tools.exe"
set "D=%~dp0"
set "TMP=%ROOT%\build\%BACKEND%"

if not exist "%EXE%" (
    echo [ERROR] nav-tools not built: build\%BACKEND%\nav-tools.exe
    echo         build via scripts\build-m0.2.bat %BACKEND%
    exit /b 1
)

echo === A: selftest --verbose (M-N0..M-N5 anchors) ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] nav-tools selftest FAILED
    exit /b 1
)
findstr /c:"M-N5: bake with offmesh link" "%D%selftest.log" >nul || (echo [ERROR] M-N5 bake anchor missing & exit /b 1)
findstr /c:"M-N5: v3 bytes identical" "%D%selftest.log" >nul || (echo [ERROR] M-N5 v3 roundtrip anchor missing & exit /b 1)
findstr /c:"M-N5: straight path has OFFMESH flag" "%D%selftest.log" >nul || (echo [ERROR] M-N5 jump flag anchor missing & exit /b 1)
findstr /c:"M-N5: exclude flag blocks link" "%D%selftest.log" >nul || (echo [ERROR] M-N5 exclude anchor missing & exit /b 1)
findstr /c:"M-N5: one-way B->A unreachable" "%D%selftest.log" >nul || (echo [ERROR] M-N5 direction anchor missing & exit /b 1)
findstr /c:"M-N5 negative: endpoint outside bbox rejected" "%D%selftest.log" >nul || (echo [ERROR] M-N5 bbox negative anchor missing & exit /b 1)
echo A: selftest OK

echo === B: bridge link - bake v3 + query + PNG evidence ===
"%EXE%" offmesh --nav "%D%offmesh-bridge.nnav" --png "%D%offmesh-bridge.png" --size 768 > "%D%bridge.log" 2>&1
if errorlevel 1 (type "%D%bridge.log" & echo [ERROR] bridge offmesh FAILED & exit /b 1)
findstr /c:"nnav version: 3" "%D%bridge.log" >nul || (echo [ERROR] v3 version anchor missing & exit /b 1)
findstr /c:"offmesh poly: 1" "%D%bridge.log" >nul || (echo [ERROR] retained link poly anchor missing & exit /b 1)
findstr /c:"roundtrip   : identical" "%D%bridge.log" >nul || (echo [ERROR] v3 resave bytes anchor missing & exit /b 1)
findstr /c:"status      : OK" "%D%bridge.log" >nul || (echo [ERROR] cross-gap reachable anchor missing & exit /b 1)
findstr /c:"offmesh segs: 1" "%D%bridge.log" >nul || (echo [ERROR] straight-path jump segment anchor missing & exit /b 1)
findstr /c:"result      : PASS" "%D%bridge.log" >nul || (echo [ERROR] result anchor missing & exit /b 1)
if not exist "%D%offmesh-bridge.nnav" (echo [ERROR] missing offmesh-bridge.nnav & exit /b 1)
if not exist "%D%offmesh-bridge.png" (echo [ERROR] missing offmesh-bridge.png & exit /b 1)
type "%D%bridge.log"

echo === C: determinism - second run .nnav and PNG byte-identical ===
"%EXE%" offmesh --nav "%TMP%\nav-n5-bridge-b.nnav" --png "%TMP%\nav-n5-bridge-b.png" --size 768 > "%TMP%\nav-n5-bridge-b.log" 2>&1
if errorlevel 1 (type "%TMP%\nav-n5-bridge-b.log" & echo [ERROR] second offmesh FAILED & exit /b 1)
fc /b "%D%offmesh-bridge.nnav" "%TMP%\nav-n5-bridge-b.nnav" >nul || (echo [ERROR] determinism: .nnav bytes differ & exit /b 1)
fc /b "%D%offmesh-bridge.png" "%TMP%\nav-n5-bridge-b.png" >nul || (echo [ERROR] determinism: PNG bytes differ & exit /b 1)
echo C: .nnav + PNG fc /b byte-identical OK
del "%TMP%\nav-n5-bridge-b.nnav"
del "%TMP%\nav-n5-bridge-b.png"
del "%TMP%\nav-n5-bridge-b.log"

echo === D: semantics - no link / exclude / one-way / reverse ===
"%EXE%" offmesh --no-link --nav "%TMP%\nav-n5-nolink.nnav" > "%D%nolink.log" 2>&1
if errorlevel 1 (type "%D%nolink.log" & echo [ERROR] no-link case FAILED & exit /b 1)
findstr /c:"nnav version: 1" "%D%nolink.log" >nul || (echo [ERROR] no-link must stay v1 & exit /b 1)
findstr /c:"status      : UNREACHABLE" "%D%nolink.log" >nul || (echo [ERROR] no-link must be unreachable & exit /b 1)
findstr /c:"result      : PASS" "%D%nolink.log" >nul || (echo [ERROR] no-link result anchor missing & exit /b 1)
"%EXE%" offmesh --exclude --nav "%TMP%\nav-n5-exclude.nnav" > "%D%exclude.log" 2>&1
if errorlevel 1 (type "%D%exclude.log" & echo [ERROR] exclude case FAILED & exit /b 1)
findstr /c:"exclude     : 1" "%D%exclude.log" >nul || (echo [ERROR] exclude flag anchor missing & exit /b 1)
findstr /c:"status      : UNREACHABLE" "%D%exclude.log" >nul || (echo [ERROR] exclude must block link & exit /b 1)
findstr /c:"result      : PASS" "%D%exclude.log" >nul || (echo [ERROR] exclude result anchor missing & exit /b 1)
"%EXE%" offmesh --one-way --nav "%TMP%\nav-n5-oneway.nnav" > "%D%oneway.log" 2>&1
if errorlevel 1 (type "%D%oneway.log" & echo [ERROR] one-way case FAILED & exit /b 1)
findstr /c:"status      : OK" "%D%oneway.log" >nul || (echo [ERROR] one-way A->B must be reachable & exit /b 1)
"%EXE%" offmesh --one-way --reverse --nav "%TMP%\nav-n5-reverse.nnav" > "%D%reverse.log" 2>&1
if errorlevel 1 (type "%D%reverse.log" & echo [ERROR] reverse case FAILED & exit /b 1)
findstr /c:"status      : UNREACHABLE" "%D%reverse.log" >nul || (echo [ERROR] one-way B->A must be unreachable & exit /b 1)
echo D: no-link(v1) / exclude / one-way / reverse ALL GREEN
del "%TMP%\nav-n5-nolink.nnav"
del "%TMP%\nav-n5-exclude.nnav"
del "%TMP%\nav-n5-oneway.nnav"
del "%TMP%\nav-n5-reverse.nnav"

echo === E: zero regression - v1 solo bake still byte-identical to M-N0 baseline ===
"%EXE%" bake "%ROOT%\assets\models\HizScene.glb" "%TMP%\nav-n5-v1check.nnav" --cell-size 0.1 --agent-radius 0.2 > "%D%v1zero.log" 2>&1
if errorlevel 1 (type "%D%v1zero.log" & echo [ERROR] v1 bake FAILED & exit /b 1)
fc /b "%TMP%\nav-n5-v1check.nnav" "%ROOT%\example\nav-m0\hiz-a.nnav" >nul || (echo [ERROR] v1 baseline bytes changed & exit /b 1)
del "%TMP%\nav-n5-v1check.nnav"
echo E: 864 B v1 baseline unchanged OK

echo === F: negatives - radius / bbox / usage ===
"%EXE%" offmesh --radius 0 --nav "%TMP%\nav-n5-radius.nnav" > "%D%radius0.log" 2>&1
if errorlevel 2 (echo F1: radius=0 exit 2 OK) else (echo [ERROR] radius=0 should exit 2 & exit /b 1)
"%EXE%" offmesh --link "1,0,0 200,0,0" --nav "%TMP%\nav-n5-bbox.nnav" > "%D%bbox.log" 2>&1
if errorlevel 2 (echo [ERROR] bbox rejection should exit 1 not 2 & exit /b 1)
if errorlevel 1 (echo F2: bbox-out endpoint exit 1 OK) else (type "%D%bbox.log" & echo [ERROR] bbox-out must be rejected & exit /b 1)
findstr /c:"invalid off-mesh connection #0" "%D%bbox.log" >nul || (echo [ERROR] explicit rejection anchor missing & exit /b 1)
"%EXE%" offmesh --bogus > "%D%usage.log" 2>&1
if errorlevel 2 (echo F3: unknown option exit 2 OK) else (echo [ERROR] unknown option should exit 2 & exit /b 1)

echo === NAV-N5 ACCEPTANCE: ALL GREEN [%BACKEND%] ===
exit /b 0

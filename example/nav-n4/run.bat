@echo off
rem P8 M-N4 acceptance: DetourCrowd fixed-step multi-agent trace
rem Q1=A tool card / Q2=A-1 .nnav unchanged / Q3=A fixed dt+frames+order.
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

echo === A: selftest --verbose (M-N0..M-N4 anchors) ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] nav-tools selftest FAILED
    exit /b 1
)
findstr /c:"crowd: deterministic trace point-by-point" "%D%selftest.log" >nul || (echo [ERROR] crowd determinism anchor missing & exit /b 1)
findstr /c:"crowd: .ntraj byte-identical" "%D%selftest.log" >nul || (echo [ERROR] ntraj byte anchor missing & exit /b 1)
findstr /c:"crowd: all 4 agents reach their targets" "%D%selftest.log" >nul || (echo [ERROR] arrival anchor missing & exit /b 1)
findstr /c:"crowd: agents never overlap" "%D%selftest.log" >nul || (echo [ERROR] RVO separation anchor missing & exit /b 1)
findstr /c:"crowd: v1/v2 both yield full-length" "%D%selftest.log" >nul || (echo [ERROR] A-1 container anchor missing & exit /b 1)
findstr /c:"crowd: off-mesh spawn rejected" "%D%selftest.log" >nul || (echo [ERROR] off-mesh negative anchor missing & exit /b 1)
echo A: selftest OK

echo === B: arena crossing - 4 agents, 600 fixed frames, .ntraj + PNG ===
"%EXE%" crowd --arena --frames 600 --trace "%D%crowd-arena.ntraj" --png "%D%crowd-arena.png" --size 768 > "%D%arena.log" 2>&1
if errorlevel 1 (type "%D%arena.log" & echo [ERROR] arena crowd FAILED & exit /b 1)
findstr /c:"mode         : solo (v1)" "%D%arena.log" >nul || (echo [ERROR] arena mode anchor missing & exit /b 1)
findstr /c:"agents       : 4" "%D%arena.log" >nul || (echo [ERROR] agent count anchor missing & exit /b 1)
findstr /c:"frames       : 601" "%D%arena.log" >nul || (echo [ERROR] fixed frame count anchor missing & exit /b 1)
findstr /c:"dt           : 0.016667" "%D%arena.log" >nul || (echo [ERROR] fixed dt anchor missing & exit /b 1)
findstr /c:"arrived      : 4 / 4" "%D%arena.log" >nul || (echo [ERROR] all agents must arrive & exit /b 1)
findstr /c:"minPairDist  : 0.60" "%D%arena.log" >nul || (echo [ERROR] RVO separation distance anchor missing & exit /b 1)
findstr /c:"result       : PASS" "%D%arena.log" >nul || (echo [ERROR] result anchor missing & exit /b 1)
if not exist "%D%crowd-arena.ntraj" (echo [ERROR] missing crowd-arena.ntraj & exit /b 1)
if not exist "%D%crowd-arena.png" (echo [ERROR] missing crowd-arena.png & exit /b 1)
type "%D%arena.log"

echo === C: determinism - second run .ntraj and PNG byte-identical ===
"%EXE%" crowd --arena --frames 600 --trace "%TMP%\nav-n4-arena-b.ntraj" --png "%TMP%\nav-n4-arena-b.png" --size 768 > "%TMP%\nav-n4-arena-b.log" 2>&1
if errorlevel 1 (type "%TMP%\nav-n4-arena-b.log" & echo [ERROR] second crowd FAILED & exit /b 1)
fc /b "%D%crowd-arena.ntraj" "%TMP%\nav-n4-arena-b.ntraj" >nul || (echo [ERROR] determinism: .ntraj bytes differ & exit /b 1)
fc /b "%D%crowd-arena.png" "%TMP%\nav-n4-arena-b.png" >nul || (echo [ERROR] determinism: PNG bytes differ & exit /b 1)
echo C: .ntraj + PNG fc /b byte-identical OK
del "%TMP%\nav-n4-arena-b.ntraj"
del "%TMP%\nav-n4-arena-b.png"
del "%TMP%\nav-n4-arena-b.log"

echo === D: A-1 compatibility - v1 solo and v2 tiled .nnav both consumed ===
"%EXE%" crowd "%ROOT%\example\nav-n3\hiz-v1.nnav" --spawn -1.9,0.2,-2.1:-1.0,0.2,-1.0 --frames 60 > "%D%v1.log" 2>&1
if errorlevel 1 (type "%D%v1.log" & echo [ERROR] v1 crowd FAILED & exit /b 1)
findstr /c:"mode         : solo (v1)" "%D%v1.log" >nul || (echo [ERROR] v1 mode anchor missing & exit /b 1)
findstr /c:"arrived      : 1 / 1" "%D%v1.log" >nul || (echo [ERROR] v1 arrival anchor missing & exit /b 1)
findstr /c:"result       : PASS" "%D%v1.log" >nul || (echo [ERROR] v1 result anchor missing & exit /b 1)
"%EXE%" crowd "%ROOT%\example\nav-n3\hiz-tiled.nnav" --spawn -1.9,0.2,-2.1:-1.0,0.2,-1.0 --frames 60 > "%D%v2.log" 2>&1
if errorlevel 1 (type "%D%v2.log" & echo [ERROR] v2 crowd FAILED & exit /b 1)
findstr /c:"mode         : tiled (v2)" "%D%v2.log" >nul || (echo [ERROR] v2 mode anchor missing & exit /b 1)
findstr /c:"arrived      : 1 / 1" "%D%v2.log" >nul || (echo [ERROR] v2 arrival anchor missing & exit /b 1)
findstr /c:"result       : PASS" "%D%v2.log" >nul || (echo [ERROR] v2 result anchor missing & exit /b 1)
echo D: v1/v2 A-1 OK (no .nnav format change)

echo === E: preset line - 4 agents in formation ===
"%EXE%" crowd --arena --preset line --frames 600 > "%D%line.log" 2>&1
if errorlevel 1 (type "%D%line.log" & echo [ERROR] line preset FAILED & exit /b 1)
findstr /c:"agents       : 4" "%D%line.log" >nul || (echo [ERROR] line agent count anchor missing & exit /b 1)
findstr /c:"arrived      : 4 / 4" "%D%line.log" >nul || (echo [ERROR] line arrival anchor missing & exit /b 1)
findstr /c:"result       : PASS" "%D%line.log" >nul || (echo [ERROR] line result anchor missing & exit /b 1)
echo E: line preset OK

echo === F: negatives - usage / bad dt / off-mesh spawn / bad navmesh / over capacity ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (echo F1: global usage exit 2 OK) else (echo [ERROR] no-args should exit 2 & exit /b 1)
findstr /c:"nav-tools crowd" "%D%usage.log" >nul || (echo [ERROR] usage missing crowd line & exit /b 1)
"%EXE%" crowd --arena --dt 0 > "%D%dt0.log" 2>&1
if errorlevel 2 (echo F2: dt=0 exit 2 OK) else (type "%D%dt0.log" & echo [ERROR] dt=0 should exit 2 & exit /b 1)
"%EXE%" crowd --arena --spawn 100,0,100:-8,0.2,-8 --frames 30 > "%D%offmesh.log" 2>&1
if errorlevel 2 (echo [ERROR] off-mesh should exit 1 not 2 & exit /b 1)
if errorlevel 1 (echo F3: off-mesh exit 1 OK) else (type "%D%offmesh.log" & echo [ERROR] off-mesh must be rejected & exit /b 1)
findstr /c:"not on navmesh" "%D%offmesh.log" >nul || (echo [ERROR] off-mesh rejection anchor missing & exit /b 1)
"%EXE%" crowd "%ROOT%\assets\models\Suzanne.glb" --spawn -8,0.2,-8:8,0.2,8 --frames 30 > "%D%badnav.log" 2>&1
if errorlevel 2 (echo [ERROR] bad nav should exit 1 not 2 & exit /b 1)
if errorlevel 1 (echo F4: bad navmesh exit 1 OK) else (type "%D%badnav.log" & echo [ERROR] bad nav must be rejected & exit /b 1)
"%EXE%" crowd --arena --max-agents 2 --frames 30 > "%D%overcap.log" 2>&1
if errorlevel 2 (echo F5: over capacity exit 2 OK) else (type "%D%overcap.log" & echo [ERROR] over capacity should exit 2 & exit /b 1)

echo === NAV-N4 ACCEPTANCE: ALL GREEN [%BACKEND%] ===
exit /b 0

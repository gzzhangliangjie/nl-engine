@echo off
rem P8 M-N6 acceptance: Q1=B E1 ECS NavAgent + NavAgentSystem + viewer --nav-crowd
rem Q6-1 fixed-step ECS / Q6-2 standalone viewer canvas / Q6-3 new card + run.bat verify.
rem ASCII only. Exit 0 = all green. Arg 1 = DX12 (default) or VULKAN.
setlocal
set "BACKEND=%~1"
if "%BACKEND%"=="" set "BACKEND=DX12"
set "ROOT=%~dp0..\.."
set "TOOL=%ROOT%\build\%BACKEND%\nav-tools.exe"
set "VIEWER=%ROOT%\build\%BACKEND%\viewer.exe"
set "D=%~dp0"
set "TMP=%ROOT%\build\%BACKEND%"

if not exist "%TOOL%" (
    echo [ERROR] nav-tools not built: build\%BACKEND%\nav-tools.exe
    exit /b 1
)
if not exist "%VIEWER%" (
    echo [ERROR] viewer not built: build\%BACKEND%\viewer.exe
    exit /b 1
)

echo === A: selftest --verbose (M-N0..M-N5 zero regression + M-N6 anchors) ===
"%TOOL%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] nav-tools selftest FAILED
    exit /b 1
)
findstr /c:"M-N6: NavAgent serialise roundtrip" "%D%selftest.log" >nul || (echo [ERROR] ECS component roundtrip anchor missing & exit /b 1)
findstr /c:"M-N6: fixed-step accumulator exactly 600" "%D%selftest.log" >nul || (echo [ERROR] fixed-step anchor missing & exit /b 1)
findstr /c:"M-N6: Transform writeback 4/4" "%D%selftest.log" >nul || (echo [ERROR] Transform writeback anchor missing & exit /b 1)
findstr /c:"M-N6: ECS trace == Crowd::run point-by-point" "%D%selftest.log" >nul || (echo [ERROR] Q1=B vs M-N4 parity anchor missing & exit /b 1)
findstr /c:"M-N6: .ntraj byte-identical" "%D%selftest.log" >nul || (echo [ERROR] determinism anchor missing & exit /b 1)
findstr /c:"M-N6 negative: off-mesh spawn rejected" "%D%selftest.log" >nul || (echo [ERROR] negative anchor missing & exit /b 1)
echo A: selftest OK

echo === B: ecs-crowd - 4 ECS agents, 600 fixed steps, .ntraj + PNG ===
"%TOOL%" ecs-crowd --frames 600 --trace "%D%arena-crowd.ntraj" --png "%D%arena-crowd.png" --size 768 > "%D%ecs-crowd.log" 2>&1
if errorlevel 1 (type "%D%ecs-crowd.log" & echo [ERROR] ecs-crowd FAILED & exit /b 1)
findstr /c:"world        : entities=4 systems=1" "%D%ecs-crowd.log" >nul || (echo [ERROR] ECS world anchor missing & exit /b 1)
findstr /c:"component    : NavAgent + Transform (E1 ECS)" "%D%ecs-crowd.log" >nul || (echo [ERROR] ECS component anchor missing & exit /b 1)
findstr /c:"dt           : 0.016667 (fixed, accumulator)" "%D%ecs-crowd.log" >nul || (echo [ERROR] fixed dt anchor missing & exit /b 1)
findstr /c:"agents       : 4" "%D%ecs-crowd.log" >nul || (echo [ERROR] agent count anchor missing & exit /b 1)
findstr /c:"steps        : 600" "%D%ecs-crowd.log" >nul || (echo [ERROR] step count anchor missing & exit /b 1)
findstr /c:"frames       : 601" "%D%ecs-crowd.log" >nul || (echo [ERROR] frame count anchor missing & exit /b 1)
findstr /c:"arrived      : 4 / 4" "%D%ecs-crowd.log" >nul || (echo [ERROR] all agents must arrive & exit /b 1)
findstr /c:"transform    : writeback 4 / 4" "%D%ecs-crowd.log" >nul || (echo [ERROR] Transform writeback anchor missing & exit /b 1)
findstr /c:"minPairDist  : 0.6003" "%D%ecs-crowd.log" >nul || (echo [ERROR] RVO separation anchor missing & exit /b 1)
findstr /c:"result       : PASS" "%D%ecs-crowd.log" >nul || (echo [ERROR] result anchor missing & exit /b 1)
if not exist "%D%arena-crowd.ntraj" (echo [ERROR] missing arena-crowd.ntraj & exit /b 1)
if not exist "%D%arena-crowd.png" (echo [ERROR] missing arena-crowd.png & exit /b 1)
type "%D%ecs-crowd.log"

echo === C: determinism - second run bytes identical, and == M-N4 A-class baseline ===
"%TOOL%" ecs-crowd --frames 600 --trace "%TMP%\nav-n6-ecs-b.ntraj" --png "%TMP%\nav-n6-ecs-b.png" --size 768 > "%TMP%\nav-n6-ecs-b.log" 2>&1
if errorlevel 1 (type "%TMP%\nav-n6-ecs-b.log" & echo [ERROR] second ecs-crowd FAILED & exit /b 1)
fc /b "%D%arena-crowd.ntraj" "%TMP%\nav-n6-ecs-b.ntraj" >nul || (echo [ERROR] determinism: .ntraj bytes differ & exit /b 1)
fc /b "%D%arena-crowd.png" "%TMP%\nav-n6-ecs-b.png" >nul || (echo [ERROR] determinism: PNG bytes differ & exit /b 1)
fc /b "%D%arena-crowd.ntraj" "%ROOT%\example\nav-n4\crowd-arena.ntraj" >nul || (echo [ERROR] Q1=B trace must equal M-N4 A-class baseline & exit /b 1)
echo C: .ntraj + PNG fc /b OK; ECS trace == M-N4 Crowd::run byte-identical
del "%TMP%\nav-n6-ecs-b.ntraj"
del "%TMP%\nav-n6-ecs-b.png"
del "%TMP%\nav-n6-ecs-b.log"

echo === D: viewer --nav-crowd standalone 2D canvas (Q1=B visible multi-agent) ===
"%VIEWER%" --nav-crowd --frames 8 --nav-crowd-steps 75 --size 1280x720 --screenshot "%D%viewer-crowd-%BACKEND%.png" > "%D%viewer-crowd.log" 2>&1
if errorlevel 1 (type "%D%viewer-crowd.log" & echo [ERROR] viewer --nav-crowd FAILED & exit /b 1)
findstr /c:"nav-crowd: ECS agents=4 systems=1" "%D%viewer-crowd.log" >nul || (echo [ERROR] viewer ECS init anchor missing & exit /b 1)
findstr /c:"nav-crowd: final agents=4 steps=600 arrived=4" "%D%viewer-crowd.log" >nul || (echo [ERROR] viewer fixed-step anchor missing & exit /b 1)
findstr /c:"minPairDist=0.6003" "%D%viewer-crowd.log" >nul || (echo [ERROR] viewer RVO anchor missing & exit /b 1)
findstr /c:"screenshot written:" "%D%viewer-crowd.log" >nul || (echo [ERROR] viewer screenshot anchor missing & exit /b 1)
if not exist "%D%viewer-crowd-%BACKEND%.png" (echo [ERROR] missing viewer-crowd-%BACKEND%.png & exit /b 1)
echo D: viewer crowd canvas OK [%BACKEND%]

echo === E: scale anchor - 8 ECS agents in formation ===
"%TOOL%" ecs-crowd --agents 8 --frames 600 > "%D%agents8.log" 2>&1
if errorlevel 1 (type "%D%agents8.log" & echo [ERROR] 8-agent ECS FAILED & exit /b 1)
findstr /c:"agents       : 8" "%D%agents8.log" >nul || (echo [ERROR] 8-agent count anchor missing & exit /b 1)
findstr /c:"steps        : 600" "%D%agents8.log" >nul || (echo [ERROR] 8-agent step anchor missing & exit /b 1)
findstr /c:"transform    : writeback 8 / 8" "%D%agents8.log" >nul || (echo [ERROR] 8-agent writeback anchor missing & exit /b 1)
findstr /c:"result       : PASS" "%D%agents8.log" >nul || (echo [ERROR] 8-agent result anchor missing & exit /b 1)
echo E: 8-agent ECS OK

echo === F: negatives - bad spawn / agents 0 / dt 0 / unknown option / viewer bounds ===
"%TOOL%" ecs-crowd --bad-spawn --frames 30 > "%D%badspawn.log" 2>&1
if errorlevel 2 (echo [ERROR] bad spawn should exit 1 not 2 & exit /b 1)
if errorlevel 1 (echo F1: bad spawn exit 1 OK) else (type "%D%badspawn.log" & echo [ERROR] bad spawn must be rejected & exit /b 1)
findstr /c:"not on navmesh" "%D%badspawn.log" >nul || (echo [ERROR] bad spawn rejection anchor missing & exit /b 1)
"%TOOL%" ecs-crowd --agents 0 > "%D%agents0.log" 2>&1
if errorlevel 2 (echo F2: agents=0 exit 2 OK) else (echo [ERROR] agents=0 should exit 2 & exit /b 1)
"%TOOL%" ecs-crowd --dt 0 > "%D%dt0.log" 2>&1
if errorlevel 2 (echo F3: dt=0 exit 2 OK) else (echo [ERROR] dt=0 should exit 2 & exit /b 1)
"%TOOL%" ecs-crowd --bogus > "%D%usage.log" 2>&1
if errorlevel 2 (echo F4: unknown option exit 2 OK) else (echo [ERROR] unknown option should exit 2 & exit /b 1)
findstr /c:"unknown option" "%D%usage.log" >nul || (echo [ERROR] unknown option anchor missing & exit /b 1)
"%VIEWER%" --nav-crowd-agents 9 > "%D%viewer-neg.log" 2>&1
if errorlevel 1 (echo F5: viewer agent bound exit 1 OK) else (echo [ERROR] viewer agent bound should exit 1 & exit /b 1)

echo === NAV-N6 ACCEPTANCE: ALL GREEN [%BACKEND%] ===
exit /b 0

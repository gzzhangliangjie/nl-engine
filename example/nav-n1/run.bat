@echo off
rem P8 M-N1 acceptance: Detour query (findNearestPoly -> findPath ->
rem findStraightPath) over the M-N0 HizScene bake. ASCII only.
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

echo === selftest --verbose (bake + M-N1 query anchors) ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] nav-tools selftest FAILED
    exit /b 1
)
type "%D%selftest.log"

echo === bake HizScene.glb (cs=0.1, agent radius=0.2) ===
"%EXE%" bake "%ROOT%\assets\models\HizScene.glb" "%D%hiz-a.nnav" --cell-size 0.1 --agent-radius 0.2 > "%D%bake.log" 2>&1
if errorlevel 1 (
    type "%D%bake.log"
    echo [ERROR] bake FAILED
    exit /b 1
)
type "%D%bake.log"

echo === query reachable (OK expected, path PNG evidence) ===
"%EXE%" query "%D%hiz-a.nnav" --start -1.9,0,-2.1 --end -1.0,0,-1.0 --png "%D%query-path.png" --size 512 > "%D%query-ok.log" 2>&1
if errorlevel 1 (
    type "%D%query-ok.log"
    echo [ERROR] reachable query should exit 0
    exit /b 1
)
type "%D%query-ok.log"
if not exist "%D%query-path.png" (echo [ERROR] missing query-path.png & exit /b 1)

echo === query determinism: same query twice, path PNG byte-identical ===
"%EXE%" query "%D%hiz-a.nnav" --start -1.9,0,-2.1 --end -1.0,0,-1.0 --png "%D%query-path-b.png" --size 512 > "%D%query-ok-b.log" 2>&1
if errorlevel 1 (echo [ERROR] second query FAILED & exit /b 1)
findstr /c:"status      : OK" "%D%query-ok-b.log" >nul || (echo [ERROR] second query not OK & exit /b 1)
fc /b "%D%query-path.png" "%D%query-path-b.png" >nul || (echo [ERROR] determinism: path PNGs differ & exit /b 1)
echo determinism : query-path.png == query-path-b.png (byte-identical)
del "%D%query-path-b.png"
del "%D%query-ok-b.log"

echo === query unreachable (exit 1 expected) ===
"%EXE%" query "%D%hiz-a.nnav" --start -1.8,0,-2.0 --end 100,0,100 > "%D%query-unreachable.log" 2>&1
if errorlevel 1 if not errorlevel 2 (
    echo unreachable exit 1 OK
) else (
    echo [ERROR] unreachable query should exit 1
    exit /b 1
)
type "%D%query-unreachable.log"

echo === query usage: maxNodes below crash guard (exit 2 expected) ===
"%EXE%" query "%D%hiz-a.nnav" --start -1.8,0,-2.0 --end -1.0,0,-1.0 --max-nodes 1 > "%D%query-usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] maxNodes below guard should exit 2
    exit /b 1
)
type "%D%query-usage.log"

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === NAV-N1 ACCEPTANCE: ALL GREEN ===
exit /b 0

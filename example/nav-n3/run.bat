@echo off
rem P8 M-N3 acceptance: DetourTileCache tiled bake + .nnav v2 + dynamic obstacles
rem + E1 World JSON scene integration. ASCII only. Exit 0 = all green.
rem Build first: scripts\build-m0.2.bat DX12 (and VULKAN).
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\nav-tools.exe"
set "D=%~dp0"
set "TMP=%ROOT%\build\dx12"

if not exist "%EXE%" (
    echo [ERROR] nav-tools not built: build\dx12\nav-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest --verbose (M-N0/1/2 anchors + M-N3 tiled/v2/obstacle/scene) ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] nav-tools selftest FAILED
    exit /b 1
)
findstr /c:"tiled: path restored point-by-point" "%D%selftest.log" >nul || (echo [ERROR] tiled obstacle anchor missing & exit /b 1)
findstr /c:"scene: parent world matrix composes" "%D%selftest.log" >nul || (echo [ERROR] scene hierarchy anchor missing & exit /b 1)
type "%D%selftest.log"

echo === v1 zero-regression: solo bake bytes == example/nav-m0/hiz-a.nnav ===
"%EXE%" bake "%ROOT%\assets\models\HizScene.glb" "%D%hiz-v1.nnav" --cell-size 0.1 --agent-radius 0.2 > "%D%v1-bake.log" 2>&1
if errorlevel 1 (type "%D%v1-bake.log" & echo [ERROR] v1 solo bake FAILED & exit /b 1)
fc /b "%D%hiz-v1.nnav" "%ROOT%\example\nav-m0\hiz-a.nnav" >nul || (echo [ERROR] v1 solo bake bytes changed & exit /b 1)
echo v1 zero-regression : hiz-v1.nnav == example/nav-m0/hiz-a.nnav
type "%D%v1-bake.log"

echo === tilebake HizScene -^> v2 tiled container (2 tiles, 4 polys) ===
"%EXE%" tilebake "%ROOT%\assets\models\HizScene.glb" "%D%hiz-tiled.nnav" --tile-size 32 --cell-size 0.1 --agent-radius 0.2 --png "%D%tiles.png" --size 768 > "%D%tilebake.log" 2>&1
if errorlevel 1 (type "%D%tilebake.log" & echo [ERROR] tilebake FAILED & exit /b 1)
findstr /c:"version     : 2" "%D%tilebake.log" >nul || (echo [ERROR] v2 version anchor missing & exit /b 1)
findstr /c:"tiles       : 2" "%D%tilebake.log" >nul || (echo [ERROR] tile count anchor missing & exit /b 1)
findstr /c:"polys       : 4" "%D%tilebake.log" >nul || (echo [ERROR] poly count anchor missing & exit /b 1)
if not exist "%D%tiles.png" (echo [ERROR] missing tiles.png & exit /b 1)
type "%D%tilebake.log"

echo === determinism: tilebake twice, .nnav byte-identical ===
"%EXE%" tilebake "%ROOT%\assets\models\HizScene.glb" "%TMP%\nav-n3-tiled-b.nnav" --tile-size 32 --cell-size 0.1 --agent-radius 0.2 > "%TMP%\nav-n3-tiled-b.log" 2>&1
if errorlevel 1 (type "%TMP%\nav-n3-tiled-b.log" & echo [ERROR] second tilebake FAILED & exit /b 1)
fc /b "%D%hiz-tiled.nnav" "%TMP%\nav-n3-tiled-b.nnav" >nul || (echo [ERROR] determinism: tiled containers differ & exit /b 1)
echo determinism : hiz-tiled.nnav == second bake (byte-identical)
del "%TMP%\nav-n3-tiled-b.nnav"
del "%TMP%\nav-n3-tiled-b.log"

echo === v2 resave roundtrip: byte-identical ===
"%EXE%" resave "%D%hiz-tiled.nnav" "%TMP%\nav-n3-resave.nnav" > "%D%resave.log" 2>&1
if errorlevel 1 (type "%D%resave.log" & echo [ERROR] v2 resave FAILED & exit /b 1)
fc /b "%D%hiz-tiled.nnav" "%TMP%\nav-n3-resave.nnav" >nul || (echo [ERROR] v2 resave bytes differ & exit /b 1)
echo roundtrip : v2 save/load/save byte-identical
del "%TMP%\nav-n3-resave.nnav"
type "%D%resave.log"

echo === dump v2 container ===
"%EXE%" dump "%D%hiz-tiled.nnav" > "%D%dump.log" 2>&1
if errorlevel 1 (type "%D%dump.log" & echo [ERROR] dump FAILED & exit /b 1)
findstr /c:"polys       : 4" "%D%dump.log" >nul || (echo [ERROR] dump poly anchor missing & exit /b 1)
type "%D%dump.log"

echo === dynamic obstacle: add -^> blocked -^> remove -^> restored ===
"%EXE%" obstacle "%D%hiz-tiled.nnav" --pos -0.35,-1,-1.70 --radius 0.5 --height 3 --start -1.9,0,-2.1 --end -1.0,0,-1.0 --png "%D%obstacle.png" --size 768 > "%D%obstacle.log" 2>&1
if errorlevel 1 (type "%D%obstacle.log" & echo [ERROR] obstacle FAILED & exit /b 1)
findstr /c:"polysBefore : 4" "%D%obstacle.log" >nul || (echo [ERROR] polysBefore anchor missing & exit /b 1)
findstr /c:"polysBlocked: 3" "%D%obstacle.log" >nul || (echo [ERROR] polysBlocked anchor missing ^(tile not rebuilt^) & exit /b 1)
findstr /c:"edgesBlocked: 12" "%D%obstacle.log" >nul || (echo [ERROR] polyEdges does not match polysBlocked & exit /b 1)
findstr /c:"before      : OK" "%D%obstacle.log" >nul || (echo [ERROR] before anchor missing & exit /b 1)
findstr /c:"blocked     : UNREACHABLE" "%D%obstacle.log" >nul || (echo [ERROR] blocked anchor missing & exit /b 1)
findstr /c:"after       : OK" "%D%obstacle.log" >nul || (echo [ERROR] after anchor missing & exit /b 1)
findstr /c:"restored    : 1" "%D%obstacle.log" >nul || (echo [ERROR] restored anchor missing & exit /b 1)
findstr /c:"result      : PASS" "%D%obstacle.log" >nul || (echo [ERROR] result anchor missing & exit /b 1)
if not exist "%D%obstacle.png" (echo [ERROR] missing obstacle.png & exit /b 1)
type "%D%obstacle.log"

echo === E1 scene: World JSON -^> tiled navmesh (3 Model entities, parent hierarchy) ===
"%EXE%" tilebake "%D%nav-n3.scene" "%D%scene-tiled.nnav" --tile-size 32 --cell-size 0.1 --agent-radius 0.2 --png "%D%scene-tiles.png" --size 768 > "%D%scene.log" 2>&1
if errorlevel 1 (type "%D%scene.log" & echo [ERROR] scene tilebake FAILED & exit /b 1)
findstr /c:"entities    : 3" "%D%scene.log" >nul || (echo [ERROR] scene entity anchor missing & exit /b 1)
findstr /c:"verts/tris  : 3972 / 6948" "%D%scene.log" >nul || (echo [ERROR] scene geometry anchor missing & exit /b 1)
findstr /c:"tiles       : 8" "%D%scene.log" >nul || (echo [ERROR] scene tile anchor missing & exit /b 1)
findstr /c:"polys       : 12" "%D%scene.log" >nul || (echo [ERROR] scene poly anchor missing & exit /b 1)
if not exist "%D%scene-tiles.png" (echo [ERROR] missing scene-tiles.png & exit /b 1)
type "%D%scene.log"

echo === E1 50-object scale anchor (ecs-e1b-scale.scene) ===
"%EXE%" tilebake "%ROOT%\example\ecs-e1b\ecs-e1b-scale.scene" "%TMP%\nav-n3-scale.nnav" --tile-size 64 > "%D%scale.log" 2>&1
if errorlevel 1 (type "%D%scale.log" & echo [ERROR] 50-object tilebake FAILED & exit /b 1)
findstr /c:"entities    : 50" "%D%scale.log" >nul || (echo [ERROR] 50-object anchor missing & exit /b 1)
del "%TMP%\nav-n3-scale.nnav"
type "%D%scale.log"

echo === negatives: bad tile size (exit 2) / obstacle missing args (exit 2) / v1 rejected (exit 1) ===
"%EXE%" tilebake "%ROOT%\assets\models\HizScene.glb" "%TMP%\nav-n3-bad.nnav" --tile-size 1 > "%D%tile-usage.log" 2>&1
if errorlevel 2 (echo tile-size exit 2 OK) else (type "%D%tile-usage.log" & echo [ERROR] bad --tile-size should exit 2 & exit /b 1)
"%EXE%" obstacle "%D%hiz-tiled.nnav" > "%D%obstacle-usage.log" 2>&1
if errorlevel 2 (echo obstacle usage exit 2 OK) else (type "%D%obstacle-usage.log" & echo [ERROR] missing args should exit 2 & exit /b 1)
"%EXE%" obstacle "%D%hiz-v1.nnav" --pos -0.35,-1,-1.70 --start -1.9,0,-2.1 --end -1.0,0,-1.0 > "%D%obstacle-v1.log" 2>&1
if errorlevel 2 (type "%D%obstacle-v1.log" & echo [ERROR] v1 should exit 1 not 2 & exit /b 1)
if errorlevel 1 (echo v1 obstacle exit 1 OK) else (type "%D%obstacle-v1.log" & echo [ERROR] v1 should be rejected & exit /b 1)
findstr /c:"not a tiled navmesh" "%D%obstacle-v1.log" >nul || (echo [ERROR] v1 rejection anchor missing & exit /b 1)
type "%D%tile-usage.log"
type "%D%obstacle-usage.log"
type "%D%obstacle-v1.log"

echo === usage (exit 2 expected, must list tilebake + obstacle) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (echo usage exit 2 OK) else (echo [ERROR] no-args should exit 2 & exit /b 1)
findstr /c:"nav-tools tilebake" "%D%usage.log" >nul || (echo [ERROR] usage missing tilebake & exit /b 1)
findstr /c:"nav-tools obstacle" "%D%usage.log" >nul || (echo [ERROR] usage missing obstacle & exit /b 1)
type "%D%usage.log"

echo === NAV-N3 ACCEPTANCE: ALL GREEN ===
exit /b 0

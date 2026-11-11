@echo off
rem P8 M-N2 acceptance: five-stage debug visualization (keepDebug intermediates
rem -> DebugPrims collector -> top-down raster PNG). ASCII only.
rem Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12 (and VULKAN).
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\nav-tools.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] nav-tools not built: build\dx12\nav-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === selftest --verbose (bake + M-N1 query + M-N2 debug anchors) ===
"%EXE%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] nav-tools selftest FAILED
    exit /b 1
)
type "%D%selftest.log"

echo === five-stage overlays from HizScene.glb (cs=0.1 radius=0.2) ===
for %%S in (voxels regions contours polymesh navmesh) do (
    "%EXE%" debug-stage "%ROOT%\assets\models\HizScene.glb" "%D%stage-%%S.png" --stage %%S --size 768 --cell-size 0.1 --agent-radius 0.2 > "%D%stage-%%S.log" 2>&1
    if errorlevel 1 (
        type "%D%stage-%%S.log"
        echo [ERROR] debug-stage %%S FAILED
        exit /b 1
    )
    findstr /c:"stage       : %%S" "%D%stage-%%S.log" >nul || (echo [ERROR] stage anchor missing: %%S & exit /b 1)
    if not exist "%D%stage-%%S.png" (echo [ERROR] missing stage-%%S.png & exit /b 1)
    type "%D%stage-%%S.log"
)

echo === determinism: voxels twice, PNG byte-identical ===
"%EXE%" debug-stage "%ROOT%\assets\models\HizScene.glb" "%D%stage-voxels-b.png" --stage voxels --size 768 --cell-size 0.1 --agent-radius 0.2 > "%D%stage-voxels-b.log" 2>&1
if errorlevel 1 (echo [ERROR] second voxels run FAILED & exit /b 1)
fc /b "%D%stage-voxels.png" "%D%stage-voxels-b.png" >nul || (echo [ERROR] determinism: voxels PNGs differ & exit /b 1)
echo determinism : stage-voxels.png == stage-voxels-b.png (byte-identical)
del "%D%stage-voxels-b.png"
del "%D%stage-voxels-b.log"

echo === debug-stage usage: unknown stage name (exit 2 expected) ===
"%EXE%" debug-stage "%ROOT%\assets\models\HizScene.glb" "%D%stage-bogus.png" --stage bogus > "%D%stage-usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] unknown --stage should exit 2
    exit /b 1
)
type "%D%stage-usage.log"

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === NAV-N2 ACCEPTANCE: ALL GREEN ===
exit /b 0

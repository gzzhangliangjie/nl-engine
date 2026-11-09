@echo off
rem P8 M-P3 acceptance: collision-shape debug wireframe (Jolt DebugRenderer).
rem Tool evidence from REAL phys-tools output + viewer captures on BOTH backends
rem (deviceErrors=0 gates). Includes M-P0 + M-P1 + M-P2 byte-identical regression.
rem Exit 0 = all green. Build first:
rem   scripts\build-m0.2.bat DX12
rem   scripts\build-m0.2.bat VULKAN
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"
set "TOOLS=%ROOT%\build\dx12\phys-tools.exe"
set "VIEW_DX12=%ROOT%\build\dx12\viewer.exe"
set "VIEW_VK=%ROOT%\build\vulkan\viewer.exe"

if not exist "%TOOLS%" (
    echo [ERROR] phys-tools not built: build\dx12\phys-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)
if not exist "%VIEW_DX12%" (
    echo [ERROR] viewer not built: build\dx12\viewer.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)
if not exist "%VIEW_VK%" (
    echo [ERROR] viewer not built: build\vulkan\viewer.exe
    echo         build via scripts\build-m0.2.bat VULKAN
    exit /b 1
)

echo === selftest --verbose (M-P0..M-P3 anchors) ===
"%TOOLS%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] phys-tools selftest FAILED
    exit /b 1
)
findstr /c:"phys-p3 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P3 anchor line & exit /b 1)
findstr /c:"phys-p2 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P2 anchor line & exit /b 1)
findstr /c:"phys-p1 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P1 anchor line & exit /b 1)
findstr /c:"phys-p0 selftest: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P0 anchor line & exit /b 1)
type "%D%selftest.log"

echo === debug --out debug.tsv (real DebugRenderer stream, 240 frames) ===
"%TOOLS%" debug --out "%D%debug.tsv" || exit /b 1
findstr /c:"nl-physics M-P3 collision-shape debug wireframe" "%D%debug.tsv" >nul || (echo [ERROR] missing M-P3 TSV header & exit /b 1)
findstr /c:"frame	t	vertices	min_y" "%D%debug.tsv" >nul || (echo [ERROR] missing TSV column header & exit /b 1)

echo === check-debug debug.tsv ===
"%TOOLS%" check-debug "%D%debug.tsv" > "%D%check-debug.log" 2>&1
if errorlevel 1 (
    type "%D%check-debug.log"
    echo [ERROR] wireframe dump re-verify FAILED
    exit /b 1
)
type "%D%check-debug.log"

echo === M-P0..M-P2 baseline byte-identical regression ===
"%TOOLS%" simulate --out "%D%drop-regen.tsv" >nul || exit /b 1
"%TOOLS%" layers --out "%D%layers-regen.tsv" >nul || exit /b 1
"%TOOLS%" stack --out "%D%stack-regen.tsv" >nul || exit /b 1
"%TOOLS%" debug --out "%D%debug-regen.tsv" >nul || exit /b 1
python "%ROOT%\tools\check-phys-regression.py" > "%D%regression.log" 2>&1
if errorlevel 1 (
    type "%D%regression.log"
    echo [ERROR] M-P0 / M-P1 / M-P2 dumps changed
    exit /b 1
)
type "%D%regression.log"

echo === viewer DX12 capture (deviceErrors must be 0) ===
"%VIEW_DX12%" --physics-debug --screenshot "%D%viewer-dx12.png" --size 800x450 --frames 240 > "%D%viewer-dx12.log" 2>&1
if errorlevel 1 (
    type "%D%viewer-dx12.log"
    echo [ERROR] DX12 physics-debug capture FAILED
    exit /b 1
)
if not exist "%D%viewer-dx12.png" (echo [ERROR] screenshot missing & exit /b 1)
findstr /c:"4 bodies" "%D%viewer-dx12.log" >nul || (echo [ERROR] missing 4-bodies log & exit /b 1)
findstr /c:"deviceErrors=0" "%D%viewer-dx12.log" >nul || (echo [ERROR] DX12 deviceErrors nonzero & exit /b 1)
type "%D%viewer-dx12.log"

echo === viewer VULKAN capture (deviceErrors must be 0) ===
"%VIEW_VK%" --physics-debug --screenshot "%D%viewer-vulkan.png" --size 800x450 --frames 240 > "%D%viewer-vulkan.log" 2>&1
if errorlevel 1 (
    type "%D%viewer-vulkan.log"
    echo [ERROR] VULKAN physics-debug capture FAILED
    exit /b 1
)
if not exist "%D%viewer-vulkan.png" (echo [ERROR] screenshot missing & exit /b 1)
findstr /c:"4 bodies" "%D%viewer-vulkan.log" >nul || (echo [ERROR] missing 4-bodies log & exit /b 1)
findstr /c:"deviceErrors=0" "%D%viewer-vulkan.log" >nul || (echo [ERROR] VULKAN deviceErrors nonzero & exit /b 1)
type "%D%viewer-vulkan.log"

echo === usage (exit 2 expected) ===
"%TOOLS%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: debug --frames 0 (exit 2 expected) ===
"%TOOLS%" debug --frames 0 > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --frames should exit 2
    exit /b 1
)
type "%D%negative.log"

echo === evidence PNGs (from real TSV + logs) ===
python "%ROOT%\tools\make-phys-p3-evidence.py" || exit /b 1

echo === PHYS-P3 ACCEPTANCE: ALL GREEN ===
exit /b 0

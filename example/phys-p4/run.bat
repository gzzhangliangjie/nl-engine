@echo off
rem P8 M-P4 acceptance: character walk-through (Jolt CharacterVirtual,
rem CharacterMovement-style sweep / ground detection / ramp + steps + steep block).
rem Tool evidence from REAL phys-tools output + viewer captures on BOTH backends
rem (deviceErrors=0 gates). Includes M-P0..M-P3 byte-identical regression (5 baselines).
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

echo === selftest --verbose (M-P0..M-P4 anchors) ===
"%TOOLS%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] phys-tools selftest FAILED
    exit /b 1
)
findstr /c:"phys-p4 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P4 anchor line & exit /b 1)
findstr /c:"phys-p3 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P3 anchor line & exit /b 1)
findstr /c:"phys-p2 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P2 anchor line & exit /b 1)
findstr /c:"phys-p1 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P1 anchor line & exit /b 1)
findstr /c:"phys-p0 selftest: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing M-P0 anchor line & exit /b 1)
type "%D%selftest.log"

echo === character --out character.tsv (CharacterVirtual walk, 1200 frames) ===
"%TOOLS%" character --frames 1200 --out "%D%character.tsv" > "%D%character.log" 2>&1
if errorlevel 1 (
    type "%D%character.log"
    echo [ERROR] character walk FAILED
    exit /b 1
)
type "%D%character.log"
findstr /c:"nl-physics M-P4 character walk-through" "%D%character.tsv" >nul || (echo [ERROR] missing M-P4 TSV header & exit /b 1)
findstr /c:"frame	t	px	py	pz" "%D%character.tsv" >nul || (echo [ERROR] missing TSV column header & exit /b 1)

echo === check-character character.tsv ===
"%TOOLS%" check-character "%D%character.tsv" > "%D%check-character.log" 2>&1
if errorlevel 1 (
    type "%D%check-character.log"
    echo [ERROR] walk dump re-verify FAILED
    exit /b 1
)
type "%D%check-character.log"

echo === M-P0..M-P3 baseline byte-identical regression (5 baselines) ===
"%TOOLS%" simulate --out "%D%drop-regen.tsv" >nul || exit /b 1
"%TOOLS%" layers --out "%D%layers-regen.tsv" >nul || exit /b 1
"%TOOLS%" stack --out "%D%stack-regen.tsv" >nul || exit /b 1
"%TOOLS%" debug --out "%D%debug-regen.tsv" >nul || exit /b 1
"%TOOLS%" character --frames 1200 --out "%D%character-regen.tsv" >nul || exit /b 1
python "%ROOT%\tools\check-phys-regression.py" > "%D%regression.log" 2>&1
if errorlevel 1 (
    type "%D%regression.log"
    echo [ERROR] M-P0..M-P3 dumps changed
    exit /b 1
)
type "%D%regression.log"

echo === viewer DX12 capture (deviceErrors must be 0) ===
"%VIEW_DX12%" --physics-debug --fixed-dt 16.7 --physics-scenario 1 --screenshot "%D%viewer-dx12.png" --size 800x450 --frames 1200 > "%D%viewer-dx12.log" 2>&1
if errorlevel 1 (
    type "%D%viewer-dx12.log"
    echo [ERROR] DX12 physics-debug capture FAILED
    exit /b 1
)
if not exist "%D%viewer-dx12.png" (echo [ERROR] screenshot missing & exit /b 1)
findstr /c:"9 static course bodies" "%D%viewer-dx12.log" >nul || (echo [ERROR] missing course-bodies log & exit /b 1)
findstr /c:"deviceErrors=0" "%D%viewer-dx12.log" >nul || (echo [ERROR] DX12 deviceErrors nonzero & exit /b 1)
type "%D%viewer-dx12.log"

echo === viewer VULKAN capture (deviceErrors must be 0) ===
"%VIEW_VK%" --physics-debug --fixed-dt 16.7 --physics-scenario 1 --screenshot "%D%viewer-vulkan.png" --size 800x450 --frames 1200 > "%D%viewer-vulkan.log" 2>&1
if errorlevel 1 (
    type "%D%viewer-vulkan.log"
    echo [ERROR] VULKAN physics-debug capture FAILED
    exit /b 1
)
if not exist "%D%viewer-vulkan.png" (echo [ERROR] screenshot missing & exit /b 1)
findstr /c:"9 static course bodies" "%D%viewer-vulkan.log" >nul || (echo [ERROR] missing course-bodies log & exit /b 1)
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

echo === negative: character --frames 0 (exit 2 expected) ===
"%TOOLS%" character --frames 0 > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative exit 2 OK
) else (
    echo [ERROR] bad --frames should exit 2
    exit /b 1
)
type "%D%negative.log"

echo === evidence PNGs (from real TSV + logs) ===
python "%ROOT%\tools\make-phys-p4-evidence.py" || exit /b 1

echo === PHYS-P4 ACCEPTANCE: ALL GREEN ===
exit /b 0

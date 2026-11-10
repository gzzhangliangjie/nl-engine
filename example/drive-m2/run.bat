@echo off
rem P8 M-D2 acceptance: headless Jolt VehicleConstraint drive + M-D1 event stream.
rem Five-stage gate; exit 0 = all green. Build first:
rem   scripts\build-m0.2.bat DX12
rem   scripts\build-m0.2.bat VULKAN
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"
set "TOOLS=%ROOT%\build\DX12\phys-tools.exe"
set "VTOOLS=%ROOT%\build\VULKAN\phys-tools.exe"

if not exist "%TOOLS%" (
    echo [ERROR] phys-tools not built: build\DX12\phys-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === [1/5] selftest --verbose (M-P0..M-P4 + j5c + contacts + ecs + vehicle) ===
"%TOOLS%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 ( type "%D%selftest.log" & echo [ERROR] selftest FAILED & exit /b 1 )
findstr /c:"phys-d2 vehicle: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing vehicle anchor & exit /b 1)
findstr /c:"phys-d1 ecs: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing ecs anchor & exit /b 1)
findstr /c:"phys-d0 contacts: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing contacts anchor & exit /b 1)
findstr /c:"phys-j5c anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing j5c anchor & exit /b 1)
findstr /c:"phys-p4 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p4 anchor & exit /b 1)
findstr /c:"phys-p3 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p3 anchor & exit /b 1)
findstr /c:"phys-p2 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p2 anchor & exit /b 1)
findstr /c:"phys-p1 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p1 anchor & exit /b 1)
findstr /c:"phys-p0 selftest: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p0 anchor & exit /b 1)
type "%D%selftest.log"

echo === [2/5] drive-trace + drive-check (byte-stable dump) ===
copy /b "%D%drive.tsv" "%D%drive.base.tsv" >nul
"%TOOLS%" drive-trace --frames 480 --out "%D%drive.tsv" > "%D%drive-trace.log" 2>&1
if errorlevel 1 ( type "%D%drive-trace.log" & echo [ERROR] drive-trace FAILED & exit /b 1 )
findstr /c:"wrote" "%D%drive-trace.log" >nul || (echo [ERROR] missing trace wrote line & exit /b 1)
fc /b "%D%drive.tsv" "%D%drive.base.tsv" >nul
if errorlevel 1 ( echo [ERROR] drive.tsv is not byte-stable & exit /b 1 )
del "%D%drive.base.tsv"
"%TOOLS%" drive-check "%D%drive.tsv" > "%D%drive-check.log" 2>&1
if errorlevel 1 ( type "%D%drive-check.log" & echo [ERROR] drive-check FAILED & exit /b 1 )
findstr /c:"phys-d2 drive-check: PASS" "%D%drive-check.log" >nul || (echo [ERROR] missing check PASS line & exit /b 1)
type "%D%drive-trace.log"
type "%D%drive-check.log"

echo === [3/5] cross-backend DX12 vs VULKAN trace byte-identical ===
if exist "%VTOOLS%" (
    "%VTOOLS%" drive-trace --frames 480 --out "%D%drive-vulkan.tsv" >nul
    if errorlevel 1 ( echo [ERROR] VULKAN drive-trace FAILED & exit /b 1 )
    fc /b "%D%drive.tsv" "%D%drive-vulkan.tsv" >nul
    if errorlevel 1 ( echo [ERROR] DX12 / VULKAN traces differ & exit /b 1 )
    del "%D%drive-vulkan.tsv"
    echo DX12 vs VULKAN drive-trace byte-identical: OK > "%D%cross-backend.log"
) else (
    echo VULKAN build absent - cross-backend check SKIPPED > "%D%cross-backend.log"
)
type "%D%cross-backend.log"

echo === [4/5] usage / negative exit codes ===
"%TOOLS%" drive-trace --frames 0 > "%D%usage.log" 2>&1
if errorlevel 2 ( echo usage exit 2 OK >> "%D%usage.log" ) else ( type "%D%usage.log" & echo [ERROR] bad --frames should exit 2 & exit /b 1 )
"%TOOLS%" drive-check "%D%no-such-file.tsv" > "%D%negative.log" 2>&1
if errorlevel 1 ( echo negative exit 1 OK >> "%D%negative.log" ) else ( type "%D%negative.log" & echo [ERROR] missing file should exit 1 & exit /b 1 )
type "%D%usage.log"
type "%D%negative.log"

echo === [5/5] evidence PNG regenerate + byte-compare ===
if not exist "%D%timeline.png" (
    python "%ROOT%\tools\make-drive-m2-evidence.py" > "%D%evidence.log" 2>&1
    if errorlevel 1 ( type "%D%evidence.log" & echo [ERROR] evidence generation FAILED & exit /b 1 )
    echo [INFO] first run: PNG baselines generated, byte-compare skipped >> "%D%evidence.log"
    type "%D%evidence.log"
    echo === DRIVE-M2 ACCEPTANCE: ALL GREEN ===
    exit /b 0
)
copy /b "%D%timeline.png" "%D%timeline.base.png" >nul
copy /b "%D%acceptance.png" "%D%acceptance.base.png" >nul
python "%ROOT%\tools\make-drive-m2-evidence.py" > "%D%evidence.log" 2>&1
if errorlevel 1 ( type "%D%evidence.log" & echo [ERROR] evidence generation FAILED & exit /b 1 )
fc /b "%D%timeline.png" "%D%timeline.base.png" >nul
if errorlevel 1 ( echo [ERROR] timeline.png is not byte-stable & exit /b 1 )
fc /b "%D%acceptance.png" "%D%acceptance.base.png" >nul
if errorlevel 1 ( echo [ERROR] acceptance.png is not byte-stable & exit /b 1 )
del "%D%timeline.base.png" "%D%acceptance.base.png"
echo evidence PNG byte-identical: OK >> "%D%evidence.log"
type "%D%evidence.log"
echo === DRIVE-M2 ACCEPTANCE: ALL GREEN ===
exit /b 0

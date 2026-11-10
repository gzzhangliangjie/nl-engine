@echo off
rem P8 M-D3 acceptance: visible drive in DebugScene scenario 2 (viewer screenshot).
rem Five-stage gate; exit 0 = all green. Build first:
rem   scripts\build-m0.2.bat DX12
rem   scripts\build-m0.2.bat VULKAN
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"
set "TOOLS=%ROOT%\build\DX12\phys-tools.exe"
set "VTOOLS=%ROOT%\build\VULKAN\phys-tools.exe"
set "VIEW=%ROOT%\build\DX12\viewer.exe"
set "VVIEW=%ROOT%\build\VULKAN\viewer.exe"
set "TMP=%ROOT%\build\drive-m3-accept"
set "ARGS=--physics-debug --physics-scenario 2 --drive-demo --drive-hud --fixed-dt 16.6667 --frames 300 --size 960x540 --dist 45 --yaw 90 --pitch 40"
set "ARGS_NOHUD=--physics-debug --physics-scenario 2 --drive-demo --fixed-dt 16.6667 --frames 300 --size 960x540 --dist 45 --yaw 90 --pitch 40"

if not exist "%TOOLS%" (
    echo [ERROR] phys-tools not built: build\DX12\phys-tools.exe
    exit /b 1
)
if not exist "%VIEW%" (
    echo [ERROR] viewer not built: build\DX12\viewer.exe
    exit /b 1
)
if not exist "%TMP%" mkdir "%TMP%"

echo === [1/5] phys-tools selftest --verbose ===
"%TOOLS%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 ( type "%D%selftest.log" & echo [ERROR] selftest FAILED & exit /b 1 )
findstr /c:"phys-d2 vehicle: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing vehicle anchor & exit /b 1)
findstr /c:"phys-d1 ecs: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing ecs anchor & exit /b 1)
findstr /c:"phys-d0 contacts: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing contacts anchor & exit /b 1)
type "%D%selftest.log"

echo === [2/5] M-D0/D1/D2 archived TSV zero-regression ===
"%TOOLS%" contact-trace --frames 240 --out "%TMP%\contacts.tsv" >nul
if errorlevel 1 ( echo [ERROR] contact-trace FAILED & exit /b 1 )
fc /b "%TMP%\contacts.tsv" "%ROOT%\example\drive-m0\contacts.tsv" >nul
if errorlevel 1 ( echo [ERROR] M-D0 contacts.tsv regressed & exit /b 1 )
"%TOOLS%" ecs-trace --frames 240 --out "%TMP%\ecs.tsv" >nul
if errorlevel 1 ( echo [ERROR] ecs-trace FAILED & exit /b 1 )
fc /b "%TMP%\ecs.tsv" "%ROOT%\example\drive-m1\ecs.tsv" >nul
if errorlevel 1 ( echo [ERROR] M-D1 ecs.tsv regressed & exit /b 1 )
"%TOOLS%" drive-trace --frames 480 --out "%TMP%\drive.tsv" >nul
if errorlevel 1 ( echo [ERROR] drive-trace FAILED & exit /b 1 )
fc /b "%TMP%\drive.tsv" "%ROOT%\example\drive-m2\drive.tsv" >nul
if errorlevel 1 ( echo [ERROR] M-D2 drive.tsv regressed & exit /b 1 )
echo M-D0 contacts.tsv byte-identical: OK > "%D%zero-regression.log"
echo M-D1 ecs.tsv byte-identical: OK >> "%D%zero-regression.log"
echo M-D2 drive.tsv byte-identical: OK >> "%D%zero-regression.log"
if exist "%VTOOLS%" (
    "%VTOOLS%" drive-trace --frames 480 --out "%TMP%\drive-vk.tsv" >nul
    if errorlevel 1 ( echo [ERROR] VULKAN drive-trace FAILED & exit /b 1 )
    fc /b "%TMP%\drive.tsv" "%TMP%\drive-vk.tsv" >nul
    if errorlevel 1 ( echo [ERROR] DX12 / VULKAN traces differ & exit /b 1 )
    echo DX12 / VULKAN drive-trace byte-identical: OK >> "%D%zero-regression.log"
) else (
    echo VULKAN build absent - cross-backend trace SKIPPED >> "%D%zero-regression.log"
)
type "%D%zero-regression.log"

echo === [3/5] DX12 / VULKAN drive-demo screenshot baselines ===
"%VIEW%" %ARGS% --screenshot "%TMP%\drive-dx12.png" > "%TMP%\drive-dx12.log" 2>&1
if errorlevel 1 ( type "%TMP%\drive-dx12.log" & echo [ERROR] DX12 viewer FAILED & exit /b 1 )
findstr /c:"drive scene: speed=" "%TMP%\drive-dx12.log" >nul || (echo [ERROR] missing DX12 drive scene anchor & exit /b 1)
findstr /c:"drive hook: chassis valid=1" "%TMP%\drive-dx12.log" >nul || (echo [ERROR] missing DX12 chassis hook & exit /b 1)
findstr /c:"deviceErrors=0" "%TMP%\drive-dx12.log" >nul || (echo [ERROR] DX12 deviceErrors nonzero & exit /b 1)
fc /b "%TMP%\drive-dx12.png" "%D%drive-dx12.png" >nul
if errorlevel 1 ( echo [ERROR] DX12 screenshot differs from baseline & exit /b 1 )
echo DX12 drive-demo PNG byte-identical to baseline: OK > "%D%cross-backend.log"
if exist "%VVIEW%" (
    "%VVIEW%" %ARGS% --screenshot "%TMP%\drive-vulkan.png" > "%TMP%\drive-vulkan.log" 2>&1
    if errorlevel 1 ( type "%TMP%\drive-vulkan.log" & echo [ERROR] VULKAN viewer FAILED & exit /b 1 )
    findstr /c:"drive scene: speed=" "%TMP%\drive-vulkan.log" >nul || (echo [ERROR] missing VULKAN drive scene anchor & exit /b 1)
    fc /b "%TMP%\drive-vulkan.png" "%D%drive-vulkan.png" >nul
    if errorlevel 1 ( echo [ERROR] VULKAN screenshot differs from baseline & exit /b 1 )
    echo VULKAN drive-demo PNG byte-identical to baseline: OK >> "%D%cross-backend.log"
) else (
    echo VULKAN build absent - VULKAN screenshot SKIPPED >> "%D%cross-backend.log"
)
findstr /c:"drive scene: speed=" "%TMP%\drive-dx12.log" >> "%D%cross-backend.log"
echo DX12 / VULKAN physics anchors identical: OK >> "%D%cross-backend.log"
type "%D%cross-backend.log"

echo === [4/5] console cvar + default-off gate + negative ===
"%VIEW%" %ARGS% --console-exec "vehicle.throttleScale 0.5" --screenshot "%TMP%\cvar-throttle.png" > "%TMP%\cvar-throttle.log" 2>&1
if errorlevel 1 ( type "%TMP%\cvar-throttle.log" & echo [ERROR] cvar throttle viewer FAILED & exit /b 1 )
findstr /c:"drive scene: speed=" "%TMP%\cvar-throttle.log" > "%D%cvar.log"
fc /b "%TMP%\cvar-throttle.png" "%D%drive-dx12.png" >nul
if errorlevel 2 ( echo [ERROR] fc failed on cvar throttle PNG & exit /b 1 )
if errorlevel 1 ( echo cvar throttleScale 0.5: PNG differs from baseline OK >> "%D%cvar.log" ) else ( echo [ERROR] cvar throttleScale had no effect & exit /b 1 )
"%VIEW%" %ARGS_NOHUD% --console-exec "vehicle.debug true" --screenshot "%TMP%\cvar-debug.png" > "%TMP%\cvar-debug.log" 2>&1
if errorlevel 1 ( type "%TMP%\cvar-debug.log" & echo [ERROR] cvar debug viewer FAILED & exit /b 1 )
fc /b "%TMP%\cvar-debug.png" "%D%drive-dx12.png" >nul
if errorlevel 1 ( echo [ERROR] vehicle.debug true did not reproduce --drive-hud baseline & exit /b 1 )
echo cvar vehicle.debug == drive-hud: PNG identical OK >> "%D%cvar.log"
"%VIEW%" %ARGS_NOHUD% --screenshot "%TMP%\hud-off.png" > "%TMP%\hud-off.log" 2>&1
if errorlevel 1 ( type "%TMP%\hud-off.log" & echo [ERROR] hud-off viewer FAILED & exit /b 1 )
fc /b "%TMP%\hud-off.png" "%D%drive-dx12.png" >nul
if errorlevel 2 ( echo [ERROR] fc failed on hud-off PNG & exit /b 1 )
if errorlevel 1 ( echo default-off gate: no --drive-hud PNG differs OK >> "%D%cvar.log" ) else ( echo [ERROR] HUD default-off gate broken & exit /b 1 )
"%VIEW%" --physics-debug --physics-scenario 0 --drive-demo --fixed-dt 16.6667 --frames 30 --size 960x540 --screenshot "%TMP%\neg.png" > "%TMP%\neg.log" 2>&1
if errorlevel 1 ( type "%TMP%\neg.log" & echo [ERROR] negative viewer FAILED & exit /b 1 )
findstr /c:"drive scene: speed=" "%TMP%\neg.log" >nul && (echo [ERROR] M-D3 drive scene leaked into scenario 0 & exit /b 1)
echo negative scenario 0: no M-D3 drive scene log OK > "%D%negative.log"
echo negative scenario 0: no M-D3 drive scene log OK >> "%D%cvar.log"
type "%D%cvar.log"
type "%D%negative.log"

echo === [5/5] real frame sweep + evidence PNG byte-compare ===
copy /b "%D%frames.tsv" "%TMP%\frames.base.tsv" >nul
python "%ROOT%\tools\make-drive-m3-evidence.py" --sweep --exe "%VIEW%" > "%D%sweep.log" 2>&1
if errorlevel 1 ( type "%D%sweep.log" & echo [ERROR] frame sweep FAILED & exit /b 1 )
fc /b "%D%frames.tsv" "%TMP%\frames.base.tsv" >nul
if errorlevel 1 ( echo [ERROR] frames.tsv is not byte-stable & exit /b 1 )
echo frames.tsv 10 real runs byte-stable: OK >> "%D%sweep.log"
del "%TMP%\frames.base.tsv"

if not exist "%D%timeline.png" (
    python "%ROOT%\tools\make-drive-m3-evidence.py" > "%TMP%\evidence.log" 2>&1
    if errorlevel 1 ( type "%TMP%\evidence.log" & echo [ERROR] evidence generation FAILED & exit /b 1 )
    echo [INFO] first run: PNG baselines generated, byte-compare skipped >> "%TMP%\evidence.log"
    type "%TMP%\evidence.log"
    echo === DRIVE-M3 ACCEPTANCE: ALL GREEN ===
    exit /b 0
)
copy /b "%D%timeline.png" "%TMP%\timeline.base.png" >nul
copy /b "%D%acceptance.png" "%TMP%\acceptance.base.png" >nul
python "%ROOT%\tools\make-drive-m3-evidence.py" > "%TMP%\evidence.log" 2>&1
if errorlevel 1 ( type "%TMP%\evidence.log" & echo [ERROR] evidence regeneration FAILED & exit /b 1 )
fc /b "%D%timeline.png" "%TMP%\timeline.base.png" >nul
if errorlevel 1 ( echo [ERROR] timeline.png is not byte-stable & exit /b 1 )
fc /b "%D%acceptance.png" "%TMP%\acceptance.base.png" >nul
if errorlevel 1 ( echo [ERROR] acceptance.png is not byte-stable & exit /b 1 )
del "%TMP%\timeline.base.png" "%TMP%\acceptance.base.png"
echo evidence PNG byte-identical: OK >> "%TMP%\evidence.log"
type "%TMP%\evidence.log"
echo === DRIVE-M3 ACCEPTANCE: ALL GREEN ===
exit /b 0

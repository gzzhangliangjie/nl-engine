@echo off
rem M-CAM5 acceptance: pending b-segment visual cards (free / chase / track / smooth+shake).
rem Five-stage gate; exit 0 = all green. Build first:
rem   scripts\build-m0.2.bat DX12
rem   scripts\build-m0.2.bat VULKAN
rem Baselines: example\cam-b\cam-<mode>-<backend>.png (committed).
rem Evidence rule: every card is CLI-deterministic (--free-demo frame script /
rem --drive-demo / --camera-track TSV / --console-exec), --fixed-dt pins dt, so
rem the PNGs are byte-reproducible per backend. Anchors come from real logs.
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"
set "TOOLS=%ROOT%\build\DX12\cam-tools.exe"
set "VTOOLS=%ROOT%\build\VULKAN\cam-tools.exe"
set "VIEW=%ROOT%\build\DX12\viewer.exe"
set "VVIEW=%ROOT%\build\VULKAN\viewer.exe"
set "TMP=%ROOT%\build\cam-b-accept"
set "TRACK=%ROOT%\example\cam-b\cam-track.tsv"
set "ARGS_FREE=--camera free --free-demo --fixed-dt 16.6667 --frames 120 --size 960x540"
set "ARGS_TRACK=--camera track --camera-track %TRACK% --fixed-dt 16.6667 --frames 300 --size 960x540"

if not exist "%TOOLS%" ( echo [ERROR] cam-tools not built: build\DX12\cam-tools.exe & exit /b 1 )
if not exist "%VIEW%"  ( echo [ERROR] viewer not built: build\DX12\viewer.exe & exit /b 1 )
if not exist "%TMP%"   mkdir "%TMP%"

echo === [1/5] cam-tools + viewer selftest (zero-regression base) ===
"%TOOLS%" selftest > "%D%selftest-dx12.log" 2>&1 || ( type "%D%selftest-dx12.log" & echo [ERROR] cam-tools selftest FAILED & exit /b 1 )
findstr /c:"selftest PASS (8 sections)" "%D%selftest-dx12.log" >nul || ( echo [ERROR] missing cam-tools selftest anchor & exit /b 1 )
"%VTOOLS%" selftest > "%D%selftest-vulkan.log" 2>&1 || ( echo [ERROR] VULKAN cam-tools selftest FAILED & exit /b 1 )
findstr /c:"selftest PASS (8 sections)" "%D%selftest-vulkan.log" >nul || ( echo [ERROR] missing VULKAN cam-tools selftest anchor & exit /b 1 )
"%VIEW%" --selftest > "%TMP%\viewer-selftest-dx12.log" 2>&1 || ( echo [ERROR] viewer selftest FAILED & exit /b 1 )
findstr /c:"SELFTEST PASS (16/16)" "%TMP%\viewer-selftest-dx12.log" >nul || ( echo [ERROR] missing viewer selftest anchor & exit /b 1 )
echo cam-tools 8 sections x2 + viewer 16/16: OK

echo === [2/5] free camera card (byte-compare both backends + rerun determinism) ===
"%VIEW%" %ARGS_FREE% --screenshot "%TMP%\free-dx12.png" > "%TMP%\free-dx12.log" 2>&1
if errorlevel 1 ( type "%TMP%\free-dx12.log" & echo [ERROR] DX12 free FAILED & exit /b 1 )
findstr /c:"cam final: mode=free" "%TMP%\free-dx12.log" >nul || ( echo [ERROR] missing DX12 free anchor & exit /b 1 )
findstr /c:"deviceErrors=0" "%TMP%\free-dx12.log" >nul || ( echo [ERROR] DX12 deviceErrors nonzero & exit /b 1 )
"%VIEW%" %ARGS_FREE% --screenshot "%TMP%\free-dx12-rerun.png" > "%TMP%\free-dx12-rerun.log" 2>&1
if errorlevel 1 ( echo [ERROR] DX12 free rerun FAILED & exit /b 1 )
fc /b "%TMP%\free-dx12-rerun.png" "%TMP%\free-dx12.png" >nul || ( echo [ERROR] free rerun differs - nondeterministic & exit /b 1 )
fc /b "%TMP%\free-dx12.png" "%D%cam-free-dx12.png" >nul || ( echo [ERROR] free DX12 differs from baseline & exit /b 1 )
if exist "%VVIEW%" (
    "%VVIEW%" %ARGS_FREE% --screenshot "%TMP%\free-vulkan.png" > "%TMP%\free-vulkan.log" 2>&1
    if errorlevel 1 ( echo [ERROR] VULKAN free FAILED & exit /b 1 )
    findstr /c:"cam final: mode=free" "%TMP%\free-vulkan.log" >nul || ( echo [ERROR] missing VULKAN free anchor & exit /b 1 )
    fc /b "%TMP%\free-vulkan.png" "%D%cam-free-vulkan.png" >nul || ( echo [ERROR] free VULKAN differs from baseline & exit /b 1 )
    findstr /c:"cam final: mode=free" "%TMP%\free-dx12.log" > "%D%anchors.log"
) else (
    echo VULKAN build absent - VULKAN free SKIPPED > "%D%anchors.log"
)
findstr /c:"cam final: mode=free" "%TMP%\free-vulkan.log" >> "%D%anchors.log" 2>nul
type "%D%anchors.log"

echo === [3/5] chase camera card (byte-compare both backends) ===
"%VIEW%" --physics-debug --physics-scenario 2 --drive-demo --camera chase --console-exec "cam.chase.height 3.5" --fixed-dt 16.6667 --frames 300 --size 960x540 --screenshot "%TMP%\chase-dx12.png" > "%TMP%\chase-dx12.log" 2>&1
if errorlevel 1 ( type "%TMP%\chase-dx12.log" & echo [ERROR] DX12 chase FAILED & exit /b 1 )
findstr /c:"cam final: mode=chase" "%TMP%\chase-dx12.log" >nul || ( echo [ERROR] missing DX12 chase anchor & exit /b 1 )
findstr /c:"drive hook: chassis valid=1" "%TMP%\chase-dx12.log" >nul || ( echo [ERROR] missing DX12 chassis hook & exit /b 1 )
findstr /c:"console exec [0] cam.chase.height 3.5 -> cam.chase.height = 3.5" "%TMP%\chase-dx12.log" >nul || ( echo [ERROR] missing DX12 cvar anchor & exit /b 1 )
findstr /c:"deviceErrors=0" "%TMP%\chase-dx12.log" >nul || ( echo [ERROR] DX12 deviceErrors nonzero & exit /b 1 )
fc /b "%TMP%\chase-dx12.png" "%D%cam-chase-dx12.png" >nul || ( echo [ERROR] chase DX12 differs from baseline & exit /b 1 )
if exist "%VVIEW%" (
    "%VVIEW%" --physics-debug --physics-scenario 2 --drive-demo --camera chase --console-exec "cam.chase.height 3.5" --fixed-dt 16.6667 --frames 300 --size 960x540 --screenshot "%TMP%\chase-vulkan.png" > "%TMP%\chase-vulkan.log" 2>&1
    if errorlevel 1 ( echo [ERROR] VULKAN chase FAILED & exit /b 1 )
    findstr /c:"cam final: mode=chase" "%TMP%\chase-vulkan.log" >nul || ( echo [ERROR] missing VULKAN chase anchor & exit /b 1 )
    fc /b "%TMP%\chase-vulkan.png" "%D%cam-chase-vulkan.png" >nul || ( echo [ERROR] chase VULKAN differs from baseline & exit /b 1 )
    findstr /c:"cam final: mode=chase" "%TMP%\chase-dx12.log" >> "%D%anchors.log"
    findstr /c:"cam final: mode=chase" "%TMP%\chase-vulkan.log" >> "%D%anchors.log"
)
echo chase card byte-identical to baselines: OK

echo === [4/5] track camera card (byte-compare both backends) ===
"%VIEW%" %ARGS_TRACK% --screenshot "%TMP%\track-dx12.png" > "%TMP%\track-dx12.log" 2>&1
if errorlevel 1 ( type "%TMP%\track-dx12.log" & echo [ERROR] DX12 track FAILED & exit /b 1 )
findstr /c:"camera track loaded: 6 keyframes, duration=5s" "%TMP%\track-dx12.log" >nul || ( echo [ERROR] missing DX12 track load anchor & exit /b 1 )
findstr /c:"cam final: mode=track" "%TMP%\track-dx12.log" >nul || ( echo [ERROR] missing DX12 track anchor & exit /b 1 )
findstr /c:"deviceErrors=0" "%TMP%\track-dx12.log" >nul || ( echo [ERROR] DX12 deviceErrors nonzero & exit /b 1 )
fc /b "%TMP%\track-dx12.png" "%D%cam-track-dx12.png" >nul || ( echo [ERROR] track DX12 differs from baseline & exit /b 1 )
if exist "%VVIEW%" (
    "%VVIEW%" %ARGS_TRACK% --screenshot "%TMP%\track-vulkan.png" > "%TMP%\track-vulkan.log" 2>&1
    if errorlevel 1 ( echo [ERROR] VULKAN track FAILED & exit /b 1 )
    findstr /c:"cam final: mode=track" "%TMP%\track-vulkan.log" >nul || ( echo [ERROR] missing VULKAN track anchor & exit /b 1 )
    fc /b "%TMP%\track-vulkan.png" "%D%cam-track-vulkan.png" >nul || ( echo [ERROR] track VULKAN differs from baseline & exit /b 1 )
    findstr /c:"cam final: mode=track" "%TMP%\track-dx12.log" >> "%D%anchors.log"
    findstr /c:"cam final: mode=track" "%TMP%\track-vulkan.log" >> "%D%anchors.log"
)
echo track card byte-identical to baselines: OK

echo === [5/5] smooth + shake card (M-CAM4; effect gates + byte-compare) ===
"%VIEW%" %ARGS_FREE% --console-exec "cam.smooth.on 1" --console-exec "cam.shake.decay 0.2" --console-exec "cam.shake.add 0.8" --screenshot "%TMP%\ss-dx12.png" > "%TMP%\ss-dx12.log" 2>&1
if errorlevel 1 ( type "%TMP%\ss-dx12.log" & echo [ERROR] DX12 smoothshake FAILED & exit /b 1 )
findstr /c:"console exec [0] cam.shake.add 0.8 -> trauma -> 0.800000" "%TMP%\ss-dx12.log" >nul || ( echo [ERROR] missing DX12 trauma anchor & exit /b 1 )
findstr /c:"cam final: mode=free" "%TMP%\ss-dx12.log" >nul || ( echo [ERROR] missing DX12 ss anchor & exit /b 1 )
fc /b "%TMP%\ss-dx12.png" "%D%cam-smoothshake-dx12.png" >nul || ( echo [ERROR] smoothshake DX12 differs from baseline & exit /b 1 )
fc /b "%TMP%\ss-dx12.png" "%TMP%\free-dx12.png" >nul
if errorlevel 1 ( echo smooth+shake PNG differs from plain free: effect visible OK >> "%D%anchors.log" ) else ( echo [ERROR] smoother/trauma had no effect & exit /b 1 )
if exist "%VVIEW%" (
    "%VVIEW%" %ARGS_FREE% --console-exec "cam.smooth.on 1" --console-exec "cam.shake.decay 0.2" --console-exec "cam.shake.add 0.8" --screenshot "%TMP%\ss-vulkan.png" > "%TMP%\ss-vulkan.log" 2>&1
    if errorlevel 1 ( echo [ERROR] VULKAN smoothshake FAILED & exit /b 1 )
    findstr /c:"cam final: mode=free" "%TMP%\ss-vulkan.log" >nul || ( echo [ERROR] missing VULKAN ss anchor & exit /b 1 )
    fc /b "%TMP%\ss-vulkan.png" "%D%cam-smoothshake-vulkan.png" >nul || ( echo [ERROR] smoothshake VULKAN differs from baseline & exit /b 1 )
    findstr /c:"cam final: mode=free" "%TMP%\ss-dx12.log" >> "%D%anchors.log"
    findstr /c:"cam final: mode=free" "%TMP%\ss-vulkan.log" >> "%D%anchors.log"
)
type "%D%anchors.log"
echo.
echo M-CAM5 ALL GREEN
exit /b 0

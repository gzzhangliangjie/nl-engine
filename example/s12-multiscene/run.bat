@echo off
setlocal
rem S12 M12.2 - multi-scene edge-preservation re-verification (hangover item).
rem ASCII only; CRLF; no parentheses in echo blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem WHY THIS EXISTS: the s12-done single convex + pure-sky scene could NOT
rem exercise the rejection layer's benefit (probeDepth_ had only 3 deviceZ
rem levels and no real geometric edges; S12-PLAN 0.8.x). This script runs the
rem SAME Q12-D / Q12-B methodology on OcclScene1M - a 1.3M-tri sphere behind
rem a front wall - where orbit motion genuinely creates disocclusion (the wall
rem reveals / hides the sphere). The new disocclusion-limited gate (check tool
rem subcommand `disocc`) proves the reject layer preserves edges specifically
rem where history is invalid, instead of the global ghosting proxy alone.
rem
rem SCOPE (respects the frozen surface): no shader change, no new frozen
rem reference image. The D-B average (Q12-D) is the temporal truth and SINGLE
rem (--temporal-off) is the no-history baseline, exactly as s12-done. Only a
rem new pure-stdlib metric is added.
rem
rem OcclScene1M camera: --yaw 0 --pitch 0 --dist 3.2 (known-good from s6 hiz).
rem Motion: --orbit-speed 1.5 sweeps yaw, revealing the occluded sphere.
rem
rem dx12/vulkan are UNROLLED on purpose: under cmd //c launched from a POSIX
rem shell, a for %%B loop is mangled to a literal %B.
rem
rem THRESHOLDS: calibrated from measurement (DX12 + VULKAN), never guessed.
rem ghost ratio limit GHOSTR and disocc ratio limit DIS_RATIO each leave
rem headroom for cross-backend variance (Q11-1). minFrac guards against a
rem vacuous mask (motion must actually expose disocclusion).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "SEQ=%TEMP%\nl-s12-multiscene"
set "TOOL=%ROOT%\tools\check-s12-done.py"
set "BASE=--model OcclScene1M.nmesh --no-hud --size 960x540 --yaw 0 --pitch 0 --dist 3.2"
set "NMOV=16"
rem Calibrated from measurement (DX12 + VULKAN, both identical within Q11-1):
rem   ghost  meanMAE(on,single)/meanMAE(off,single) = 0.810  -> GHOSTR 0.95
rem   disocc masked frac = 0.0851 (8.5% px disoccluded)
rem   disocc masked meanMAE(on,single)/meanMAE(off,single) = 0.741 -> DIS_RATIO 0.95
rem   disocc masked meanMAE(off,single) = 20.65 (OFF genuinely ghosts) -> DIS_MINGHOST 6.0
rem   DIS_MINFRAC 0.005 guards against a vacuous mask (motion must expose disocclusion)
set "GHOSTR=0.95"
set "DIS_RATIO=0.95"
set "DIS_TOL=8.0"
set "DIS_MINFRAC=0.005"
set "DIS_MINGHOST=6.0"
set FAIL=0

if not exist "%SEQ%" mkdir "%SEQ%"
if not exist "%OUT%" mkdir "%OUT%"

rem ---- DX12 ----
set "B=dx12"
set "V=%ROOT%\build\DX12\viewer.exe"

echo [shot] dx12 static smoke ON (regression catch, no metric)
"%V%" --mode filter --filter-reject --filter-gi-dump %BASE% --frames 8 --screenshot "%OUT%static-on-%B%.png" > "%OUT%verify-static-on-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 static ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 static ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-static-on-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 static ON device errors & set FAIL=1 ) else ( echo [OK] dx12 static ON deviceErrors=0 )

echo [shot] dx12 motion ON       reject layer on
"%V%" --mode filter --filter-reject --filter-gi-dump %BASE% --frames %NMOV% --orbit-speed 1.5 --screenshot "%SEQ%\on-%B%\last.png" --screenshot-seq "%SEQ%\on-%B%" > "%OUT%verify-motion-on-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 motion ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 motion ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-motion-on-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 motion ON device errors & set FAIL=1 ) else ( echo [OK] dx12 motion ON deviceErrors=0 )

echo [shot] dx12 motion OFF      reject layer off
"%V%" --mode filter --filter-gi-dump %BASE% --frames %NMOV% --orbit-speed 1.5 --screenshot "%SEQ%\off-%B%\last.png" --screenshot-seq "%SEQ%\off-%B%" > "%OUT%verify-motion-off-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 motion OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 motion OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-motion-off-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 motion OFF device errors & set FAIL=1 ) else ( echo [OK] dx12 motion OFF deviceErrors=0 )

echo [shot] dx12 motion SINGLE   same-frame no-history baseline
"%V%" --mode filter --temporal-off --filter-gi-dump %BASE% --frames %NMOV% --orbit-speed 1.5 --screenshot "%SEQ%\single-%B%\last.png" --screenshot-seq "%SEQ%\single-%B%" > "%OUT%verify-motion-single-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 motion SINGLE viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 motion SINGLE exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-motion-single-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 motion SINGLE device errors & set FAIL=1 ) else ( echo [OK] dx12 motion SINGLE deviceErrors=0 )

echo [shot] dx12 motion REJALL   over-strict threshold negative control
"%V%" --mode filter --filter-reject --reject-threshold 0.0001 --filter-gi-dump %BASE% --frames %NMOV% --orbit-speed 1.5 --screenshot "%SEQ%\rejall-%B%\last.png" --screenshot-seq "%SEQ%\rejall-%B%" > "%OUT%verify-motion-rejall-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 motion REJALL viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 motion REJALL exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-motion-rejall-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 motion REJALL device errors & set FAIL=1 ) else ( echo [OK] dx12 motion REJALL deviceErrors=0 )

echo [check] dx12 motion ghosting global proxy
python "%TOOL%" ghost "%SEQ%\on-%B%" "%SEQ%\off-%B%" "%SEQ%\single-%B%" "%SEQ%\rejall-%B%" %NMOV% %GHOSTR% > "%OUT%ghost-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 ghost & set FAIL=1 ) else ( echo [OK] dx12 ghost )
type "%OUT%ghost-%B%.log"

echo [check] dx12 disocclusion-limited edge preservation
python "%TOOL%" disocc "%SEQ%\on-%B%" "%SEQ%\off-%B%" "%SEQ%\single-%B%" "%SEQ%\rejall-%B%" %NMOV% %DIS_RATIO% %DIS_TOL% %DIS_MINFRAC% %DIS_MINGHOST% > "%OUT%disocc-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 disocc & set FAIL=1 ) else ( echo [OK] dx12 disocc )
type "%OUT%disocc-%B%.log"

rem ---- VULKAN ----
set "B=vulkan"
set "V=%ROOT%\build\VULKAN\viewer.exe"

echo [shot] vulkan static smoke ON (regression catch, no metric)
"%V%" --mode filter --filter-reject --filter-gi-dump %BASE% --frames 8 --screenshot "%OUT%static-on-%B%.png" > "%OUT%verify-static-on-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan static ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan static ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-static-on-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan static ON device errors & set FAIL=1 ) else ( echo [OK] vulkan static ON deviceErrors=0 )

echo [shot] vulkan motion ON       reject layer on
"%V%" --mode filter --filter-reject --filter-gi-dump %BASE% --frames %NMOV% --orbit-speed 1.5 --screenshot "%SEQ%\on-%B%\last.png" --screenshot-seq "%SEQ%\on-%B%" > "%OUT%verify-motion-on-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan motion ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan motion ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-motion-on-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan motion ON device errors & set FAIL=1 ) else ( echo [OK] vulkan motion ON deviceErrors=0 )

echo [shot] vulkan motion OFF      reject layer off
"%V%" --mode filter --filter-gi-dump %BASE% --frames %NMOV% --orbit-speed 1.5 --screenshot "%SEQ%\off-%B%\last.png" --screenshot-seq "%SEQ%\off-%B%" > "%OUT%verify-motion-off-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan motion OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan motion OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-motion-off-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan motion OFF device errors & set FAIL=1 ) else ( echo [OK] vulkan motion OFF deviceErrors=0 )

echo [shot] vulkan motion SINGLE   same-frame no-history baseline
"%V%" --mode filter --temporal-off --filter-gi-dump %BASE% --frames %NMOV% --orbit-speed 1.5 --screenshot "%SEQ%\single-%B%\last.png" --screenshot-seq "%SEQ%\single-%B%" > "%OUT%verify-motion-single-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan motion SINGLE viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan motion SINGLE exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-motion-single-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan motion SINGLE device errors & set FAIL=1 ) else ( echo [OK] vulkan motion SINGLE deviceErrors=0 )

echo [shot] vulkan motion REJALL   over-strict threshold negative control
"%V%" --mode filter --filter-reject --reject-threshold 0.0001 --filter-gi-dump %BASE% --frames %NMOV% --orbit-speed 1.5 --screenshot "%SEQ%\rejall-%B%\last.png" --screenshot-seq "%SEQ%\rejall-%B%" > "%OUT%verify-motion-rejall-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan motion REJALL viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan motion REJALL exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-motion-rejall-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan motion REJALL device errors & set FAIL=1 ) else ( echo [OK] vulkan motion REJALL deviceErrors=0 )

echo [check] vulkan motion ghosting global proxy
python "%TOOL%" ghost "%SEQ%\on-%B%" "%SEQ%\off-%B%" "%SEQ%\single-%B%" "%SEQ%\rejall-%B%" %NMOV% %GHOSTR% > "%OUT%ghost-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan ghost & set FAIL=1 ) else ( echo [OK] vulkan ghost )
type "%OUT%ghost-%B%.log"

echo [check] vulkan disocclusion-limited edge preservation
python "%TOOL%" disocc "%SEQ%\on-%B%" "%SEQ%\off-%B%" "%SEQ%\single-%B%" "%SEQ%\rejall-%B%" %NMOV% %DIS_RATIO% %DIS_TOL% %DIS_MINFRAC% %DIS_MINGHOST% > "%OUT%disocc-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan disocc & set FAIL=1 ) else ( echo [OK] vulkan disocc )
type "%OUT%disocc-%B%.log"

rem ---- tool selftest ----
echo [check] tool selftest
python "%TOOL%" selftest
if errorlevel 1 ( echo [FAIL] tool selftest & set FAIL=1 ) else ( echo [OK] tool selftest )

if %FAIL%==0 ( echo [ALL GREEN] s12-multiscene verify ) else ( echo [FAIL] s12-multiscene verify )
exit /b %FAIL%

@echo off
setlocal
rem S12 M12.2 - history rejection + D-B temporal reference acceptance.
rem ASCII only; CRLF; no parentheses in echo blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem Q12-D = D-B: N-frame mean of --temporal-off --filter-gi-dump shots.
rem   S12-owned and ISOMORPHIC to the object under test (same csGather
rem   weights, same jitter distribution). Jitter index is s12Frame_ % 64,
rem   so independent samples are capped at N <= 64; we use NREF=32.
rem   Motion sequences are NEVER averaged (that would blur motion); the
rem   same-frame no-history single shot is the ghosting baseline instead.
rem Q12-B = B1: no previous normal / previous_gbuffer on disk. The reject
rem   layer reads only the CURRENT frame probeDepth_ (existing texture, t2).
rem
rem BLIND CHANNEL: all shots use --dist 4 + --filter-gi-dump; at --dist 7
rem the csGather weights underflow and gatherLinear_ is exactly ZERO.
rem
rem dx12/vulkan are UNROLLED on purpose: under cmd //c launched from a POSIX
rem shell, a for %%B loop is mangled to a literal %B.
rem
rem Thresholds are CALIBRATED FROM MEASUREMENT (M12.1 precedent), not guessed.
rem
rem Assertions per backend:
rem   - every shot exits 0 and logs deviceErrors=0
rem   - avg produces the 32-frame D-B reference PNG
rem   - refcheck: ON not drifted from the D-B reference (MAE / high-freq)
rem   - negative control: a single-frame noisy shot used as ON must be rejected
rem   - ghost: motion ON closer to the no-history baseline than OFF, and the
rem     over-strict threshold REJALL rejects more history than ON
rem   - tool selftest must pass

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "SEQ=%TEMP%\nl-s12-done"
set "TOOL=%ROOT%\tools\check-s12-done.py"
set "BASE=--model Suzanne-materials.nmesh --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4"
set "NREF=32"
set "NMOV=16"
rem Calibrated from measurement (M12.1 precedent, never guessed):
rem   static MAE(OFF,D-B)=MAE(ON,D-B)=0.779, hf=0.677 both -> 1.05 / 1.00 / 1.05
rem   motion meanMAE(ON,single)=3.456, meanMAE(OFF,single)=4.086 -> ratio 0.846
rem   -> GHOSTR=0.95 leaves headroom for cross-backend variance (Q11-1).
set "MAER=1.05"
set "MAXMAE=1.00"
set "HFR=1.05"
set "GHOSTR=0.95"
set FAIL=0

if not exist "%SEQ%" mkdir "%SEQ%"
if not exist "%OUT%" mkdir "%OUT%"

rem ---- DX12 ----
set "B=dx12"
set "V=%ROOT%\build\DX12\viewer.exe"

echo [shot] dx12 D-B reference sequence %NREF% frames
"%V%" --mode filter --temporal-off --filter-gi-dump %BASE% --frames %NREF% --screenshot "%SEQ%\ref-%B%\last.png" --screenshot-seq "%SEQ%\ref-%B%" > "%OUT%verify-ref-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 ref viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 ref exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-ref-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 ref device errors & set FAIL=1 ) else ( echo [OK] dx12 ref deviceErrors=0 )

echo [check] dx12 D-B reference mean
python "%TOOL%" avg "%OUT%ref-avg-%B%.png" "%SEQ%\ref-%B%" %NREF%
if errorlevel 1 ( echo [FAIL] dx12 avg & set FAIL=1 ) else ( echo [OK] dx12 avg )

echo [shot] dx12 static OFF  reject layer off
"%V%" --mode filter --filter-gi-dump %BASE% --frames 16 --screenshot "%OUT%static-off-%B%.png" > "%OUT%verify-off-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 static OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 static OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-off-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 static OFF device errors & set FAIL=1 ) else ( echo [OK] dx12 static OFF deviceErrors=0 )

echo [shot] dx12 static ON   reject layer on
"%V%" --mode filter --filter-reject --filter-gi-dump %BASE% --frames 16 --screenshot "%OUT%static-on-%B%.png" > "%OUT%verify-on-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 static ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 static ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-on-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 static ON device errors & set FAIL=1 ) else ( echo [OK] dx12 static ON deviceErrors=0 )

echo [check] dx12 static ON vs OFF vs D-B reference
python "%TOOL%" refcheck "%OUT%ref-avg-%B%.png" "%OUT%static-off-%B%.png" "%OUT%static-on-%B%.png" %MAER% %MAXMAE% %HFR%
if errorlevel 1 ( echo [FAIL] dx12 refcheck & set FAIL=1 ) else ( echo [OK] dx12 refcheck )

echo [check] dx12 negative control - single-frame noise used as ON must be rejected
python "%TOOL%" refcheck "%OUT%ref-avg-%B%.png" "%OUT%static-off-%B%.png" "%SEQ%\ref-%B%\f00016.png" %MAER% %MAXMAE% %HFR% > "%OUT%verify-negative-%B%.log" 2>&1
if errorlevel 1 ( echo [OK] dx12 negative control rejected ) else ( echo [FAIL] dx12 negative control passed by mistake & set FAIL=1 )

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

echo [check] dx12 motion ghosting + layer response
python "%TOOL%" ghost "%SEQ%\on-%B%" "%SEQ%\off-%B%" "%SEQ%\single-%B%" "%SEQ%\rejall-%B%" %NMOV% %GHOSTR% > "%OUT%ghost-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 ghost & set FAIL=1 ) else ( echo [OK] dx12 ghost )
type "%OUT%ghost-%B%.log"

rem ---- VULKAN ----
set "B=vulkan"
set "V=%ROOT%\build\VULKAN\viewer.exe"

echo [shot] vulkan D-B reference sequence %NREF% frames
"%V%" --mode filter --temporal-off --filter-gi-dump %BASE% --frames %NREF% --screenshot "%SEQ%\ref-%B%\last.png" --screenshot-seq "%SEQ%\ref-%B%" > "%OUT%verify-ref-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan ref viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan ref exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-ref-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan ref device errors & set FAIL=1 ) else ( echo [OK] vulkan ref deviceErrors=0 )

echo [check] vulkan D-B reference mean
python "%TOOL%" avg "%OUT%ref-avg-%B%.png" "%SEQ%\ref-%B%" %NREF%
if errorlevel 1 ( echo [FAIL] vulkan avg & set FAIL=1 ) else ( echo [OK] vulkan avg )

echo [shot] vulkan static OFF  reject layer off
"%V%" --mode filter --filter-gi-dump %BASE% --frames 16 --screenshot "%OUT%static-off-%B%.png" > "%OUT%verify-off-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan static OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan static OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-off-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan static OFF device errors & set FAIL=1 ) else ( echo [OK] vulkan static OFF deviceErrors=0 )

echo [shot] vulkan static ON   reject layer on
"%V%" --mode filter --filter-reject --filter-gi-dump %BASE% --frames 16 --screenshot "%OUT%static-on-%B%.png" > "%OUT%verify-on-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan static ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan static ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-on-%B%.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan static ON device errors & set FAIL=1 ) else ( echo [OK] vulkan static ON deviceErrors=0 )

echo [check] vulkan static ON vs OFF vs D-B reference
python "%TOOL%" refcheck "%OUT%ref-avg-%B%.png" "%OUT%static-off-%B%.png" "%OUT%static-on-%B%.png" %MAER% %MAXMAE% %HFR%
if errorlevel 1 ( echo [FAIL] vulkan refcheck & set FAIL=1 ) else ( echo [OK] vulkan refcheck )

echo [check] vulkan negative control - single-frame noise used as ON must be rejected
python "%TOOL%" refcheck "%OUT%ref-avg-%B%.png" "%OUT%static-off-%B%.png" "%SEQ%\ref-%B%\f00016.png" %MAER% %MAXMAE% %HFR% > "%OUT%verify-negative-%B%.log" 2>&1
if errorlevel 1 ( echo [OK] vulkan negative control rejected ) else ( echo [FAIL] vulkan negative control passed by mistake & set FAIL=1 )

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

echo [check] vulkan motion ghosting + layer response
python "%TOOL%" ghost "%SEQ%\on-%B%" "%SEQ%\off-%B%" "%SEQ%\single-%B%" "%SEQ%\rejall-%B%" %NMOV% %GHOSTR% > "%OUT%ghost-%B%.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan ghost & set FAIL=1 ) else ( echo [OK] vulkan ghost )
type "%OUT%ghost-%B%.log"

rem ---- tool selftest ----
echo [check] tool selftest
python "%TOOL%" selftest
if errorlevel 1 ( echo [FAIL] tool selftest & set FAIL=1 ) else ( echo [OK] tool selftest )

if %FAIL%==0 ( echo [ALL GREEN] s12-done verify ) else ( echo [FAIL] s12-done verify )
exit /b %FAIL%

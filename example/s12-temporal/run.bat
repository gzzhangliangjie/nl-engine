@echo off
setlocal
rem S12 M12.0 - GI-only temporal accumulation acceptance.
rem ASCII only; CRLF; no parentheses in echo blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem ROOT CAUSE (Q12-E): M10.1 traces 64 FIXED octahedron directions per probe.
rem A static scene therefore has zero GI variance between frames, so
rem lerp(hist,cur,alpha) == cur and ON == OFF byte-identical. M12.0 adds
rem per-frame direction re-sampling in --mode filter ONLY.
rem
rem BLIND CHANNEL (M11.2, probe_atlas_dump.slang header): csGather samples only
rem the outer-normal atlas texel, which always misses in this convex+sky scene.
rem On top of that, at --dist 7 the gather weights exp2(-10000*rel*rel) all
rem underflow and gatherLinear_ is exactly ZERO. Acceptance therefore uses
rem --dist 4 (weights alive) and --filter-gi-dump (GI component direct-out).
rem The frozen PNGs in this folder are evidence only; the gate re-shoots REF
rem with the same executable so Q11-1 non-byte-exact GPU results never matter.
rem
rem dx12/vulkan are UNROLLED on purpose: under cmd //c launched from a POSIX
rem shell, a for %%B loop is mangled to a literal %B.
rem
rem Assertions per backend:
rem   - all three shots exit 0 and log deviceErrors=0
rem   - check-s12-temporal.py onoffref: ON != OFF, hf(ON) <= 0.88*hf(OFF),
rem     and MAE(ON,ref) <= 0.90*MAE(OFF,ref)
rem   - negative control: using OFF as ON must be rejected, nonzero exit
rem   - tool selftest must pass

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "MODEL=Suzanne-materials.nmesh"
set "SHOT=--model %MODEL% --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4 --frames 16"
set "HFR=0.88"
set "MAER=0.90"
set "MINDIFF=1000"
set FAIL=0

rem ---- DX12 ----
echo [shot] dx12 REF  no-jitter GI dump
"%ROOT%\build\DX12\viewer.exe" --mode probe --filter-gi-dump %SHOT% --screenshot "%OUT%verify-ref-dx12.png" > "%OUT%verify-ref-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 REF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 REF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-ref-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 REF device errors & set FAIL=1 ) else ( echo [OK] dx12 REF deviceErrors=0 )

echo [shot] dx12 OFF  single-frame jittered GI
"%ROOT%\build\DX12\viewer.exe" --mode filter --temporal-off --filter-gi-dump %SHOT% --screenshot "%OUT%verify-off-dx12.png" > "%OUT%verify-off-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-off-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 OFF device errors & set FAIL=1 ) else ( echo [OK] dx12 OFF deviceErrors=0 )

echo [shot] dx12 ON   accumulated GI
"%ROOT%\build\DX12\viewer.exe" --mode filter --filter-gi-dump %SHOT% --screenshot "%OUT%verify-on-dx12.png" > "%OUT%verify-on-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-on-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 ON device errors & set FAIL=1 ) else ( echo [OK] dx12 ON deviceErrors=0 )

echo [check] dx12 ON vs OFF vs REF
python "%ROOT%\tools\check-s12-temporal.py" onoffref "%OUT%verify-ref-dx12.png" "%OUT%verify-off-dx12.png" "%OUT%verify-on-dx12.png" %HFR% %MAER% %MINDIFF%
if errorlevel 1 ( echo [FAIL] dx12 onoffref & set FAIL=1 ) else ( echo [OK] dx12 onoffref )

rem ---- VULKAN ----
echo [shot] vulkan REF  no-jitter GI dump
"%ROOT%\build\VULKAN\viewer.exe" --mode probe --filter-gi-dump %SHOT% --screenshot "%OUT%verify-ref-vulkan.png" > "%OUT%verify-ref-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan REF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan REF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-ref-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan REF device errors & set FAIL=1 ) else ( echo [OK] vulkan REF deviceErrors=0 )

echo [shot] vulkan OFF  single-frame jittered GI
"%ROOT%\build\VULKAN\viewer.exe" --mode filter --temporal-off --filter-gi-dump %SHOT% --screenshot "%OUT%verify-off-vulkan.png" > "%OUT%verify-off-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-off-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan OFF device errors & set FAIL=1 ) else ( echo [OK] vulkan OFF deviceErrors=0 )

echo [shot] vulkan ON   accumulated GI
"%ROOT%\build\VULKAN\viewer.exe" --mode filter --filter-gi-dump %SHOT% --screenshot "%OUT%verify-on-vulkan.png" > "%OUT%verify-on-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-on-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan ON device errors & set FAIL=1 ) else ( echo [OK] vulkan ON deviceErrors=0 )

echo [check] vulkan ON vs OFF vs REF
python "%ROOT%\tools\check-s12-temporal.py" onoffref "%OUT%verify-ref-vulkan.png" "%OUT%verify-off-vulkan.png" "%OUT%verify-on-vulkan.png" %HFR% %MAER% %MINDIFF%
if errorlevel 1 ( echo [FAIL] vulkan onoffref & set FAIL=1 ) else ( echo [OK] vulkan onoffref )

rem ---- negative control + tool selftest ----
echo [check] negative control - OFF used as ON must be rejected
python "%ROOT%\tools\check-s12-temporal.py" onoffref "%OUT%verify-ref-dx12.png" "%OUT%verify-off-dx12.png" "%OUT%verify-off-dx12.png" %HFR% %MAER% %MINDIFF% > "%OUT%verify-negative.log" 2>&1
if errorlevel 1 ( echo [OK] negative control rejected ) else ( echo [FAIL] negative control passed by mistake & set FAIL=1 )

echo [check] tool selftest
python "%ROOT%\tools\check-s12-temporal.py" selftest
if errorlevel 1 ( echo [FAIL] tool selftest & set FAIL=1 ) else ( echo [OK] tool selftest )

if %FAIL%==0 ( echo [ALL GREEN] s12-temporal verify ) else ( echo [FAIL] s12-temporal verify )
exit /b %FAIL%

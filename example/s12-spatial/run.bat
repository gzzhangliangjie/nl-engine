@echo off
setlocal
rem S12 M12.1 - 5x5 joint bilateral spatial layer acceptance.
rem ASCII only; CRLF; no parentheses in echo blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem Q12-C = A: temporal -> spatial, single pass, zero new RT.
rem OFF = temporal only (spatial layer gated off, identical to M12.0),
rem ON  = temporal + 5x5 joint bilateral, REF = no-jitter GI dump.
rem
rem BLIND CHANNEL / --dist: M12.0 root causes still apply. At --dist 7 the
rem csGather weights underflow and gatherLinear_ is exactly ZERO, so all
rem shots use --dist 4 and --filter-gi-dump (GI component direct-out).
rem
rem dx12/vulkan are UNROLLED on purpose: under cmd //c launched from a POSIX
rem shell, a for %%B loop is mangled to a literal %B.
rem
rem Assertions per backend:
rem   - all three shots exit 0 and log deviceErrors=0
rem   - check-s12-spatial.py onoffref: ON != OFF, hf(ON) <= HFR*hf(OFF),
rem     MAE(ON,ref) <= MAER*MAE(OFF,ref), edgeGradRatio >= 0.30
rem     MAER is calibrated from measurement, not guessed: joint bilateral is a
rem     biased estimator, so a modest MAE increase vs REF is expected.
rem   - negative control: using OFF as ON must be rejected, nonzero exit
rem   - tool selftest must pass

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "MODEL=Suzanne-materials.nmesh"
set "SHOT=--model %MODEL% --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4 --frames 16"
set "HFR=0.90"
set "MAER=1.15"
set "MINDIFF=1000"
set FAIL=0

rem ---- DX12 ----
echo [shot] dx12 REF  no-jitter GI dump
"%ROOT%\build\DX12\viewer.exe" --mode probe --filter-gi-dump %SHOT% --screenshot "%OUT%verify-ref-dx12.png" > "%OUT%verify-ref-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 REF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 REF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-ref-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 REF device errors & set FAIL=1 ) else ( echo [OK] dx12 REF deviceErrors=0 )

echo [shot] dx12 OFF  temporal only
"%ROOT%\build\DX12\viewer.exe" --mode filter --filter-gi-dump %SHOT% --screenshot "%OUT%verify-off-dx12.png" > "%OUT%verify-off-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-off-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 OFF device errors & set FAIL=1 ) else ( echo [OK] dx12 OFF deviceErrors=0 )

echo [shot] dx12 ON   temporal plus spatial
"%ROOT%\build\DX12\viewer.exe" --mode filter --spatial --filter-gi-dump %SHOT% --screenshot "%OUT%verify-on-dx12.png" > "%OUT%verify-on-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-on-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 ON device errors & set FAIL=1 ) else ( echo [OK] dx12 ON deviceErrors=0 )

echo [check] dx12 ON vs OFF vs REF
python "%ROOT%\tools\check-s12-spatial.py" onoffref "%OUT%verify-ref-dx12.png" "%OUT%verify-off-dx12.png" "%OUT%verify-on-dx12.png" %HFR% %MAER% %MINDIFF%
if errorlevel 1 ( echo [FAIL] dx12 onoffref & set FAIL=1 ) else ( echo [OK] dx12 onoffref )

rem ---- VULKAN ----
echo [shot] vulkan REF  no-jitter GI dump
"%ROOT%\build\VULKAN\viewer.exe" --mode probe --filter-gi-dump %SHOT% --screenshot "%OUT%verify-ref-vulkan.png" > "%OUT%verify-ref-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan REF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan REF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-ref-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan REF device errors & set FAIL=1 ) else ( echo [OK] vulkan REF deviceErrors=0 )

echo [shot] vulkan OFF  temporal only
"%ROOT%\build\VULKAN\viewer.exe" --mode filter --filter-gi-dump %SHOT% --screenshot "%OUT%verify-off-vulkan.png" > "%OUT%verify-off-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-off-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan OFF device errors & set FAIL=1 ) else ( echo [OK] vulkan OFF deviceErrors=0 )

echo [shot] vulkan ON   temporal plus spatial
"%ROOT%\build\VULKAN\viewer.exe" --mode filter --spatial --filter-gi-dump %SHOT% --screenshot "%OUT%verify-on-vulkan.png" > "%OUT%verify-on-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-on-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan ON device errors & set FAIL=1 ) else ( echo [OK] vulkan ON deviceErrors=0 )

echo [check] vulkan ON vs OFF vs REF
python "%ROOT%\tools\check-s12-spatial.py" onoffref "%OUT%verify-ref-vulkan.png" "%OUT%verify-off-vulkan.png" "%OUT%verify-on-vulkan.png" %HFR% %MAER% %MINDIFF%
if errorlevel 1 ( echo [FAIL] vulkan onoffref & set FAIL=1 ) else ( echo [OK] vulkan onoffref )

rem ---- negative control + tool selftest ----
echo [check] negative control - OFF used as ON must be rejected
python "%ROOT%\tools\check-s12-spatial.py" onoffref "%OUT%verify-ref-dx12.png" "%OUT%verify-off-dx12.png" "%OUT%verify-off-dx12.png" %HFR% %MAER% %MINDIFF% > "%OUT%verify-negative.log" 2>&1
if errorlevel 1 ( echo [OK] negative control rejected ) else ( echo [FAIL] negative control passed by mistake & set FAIL=1 )

echo [check] tool selftest
python "%ROOT%\tools\check-s12-spatial.py" selftest
if errorlevel 1 ( echo [FAIL] tool selftest & set FAIL=1 ) else ( echo [OK] tool selftest )

if %FAIL%==0 ( echo [ALL GREEN] s12-spatial verify ) else ( echo [FAIL] s12-spatial verify )
exit /b %FAIL%

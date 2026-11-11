@echo off
setlocal
rem S11 M11.2 - probe-hit world-cache bounce (--probe-cache-bounce) acceptance.
rem ASCII only; CRLF; no parentheses in echo blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem ROOT CAUSE (2026-11-11, Q11-2a/d): the composite --mode probe image is
rem STRUCTURALLY blind to the M11.2 delta. csGather (probe_gather.slang) samples
rem only the atlas texel of the outer-surface normal direction; for a convex body
rem (Suzanne) against a pure-sky scene that direction always misses and returns the
rem sky texel, so the per-hit bounce rewrite of the HIT-direction texel in g_irrad
rem is never read by gather. Hence the composite --mode probe ON == OFF
rem byte-identical -- that is the EXPECTED blind-channel result, NOT a cache bug.
rem
rem The bounce fix rewrites g_irrad's HIT-DIRECTION texels (probe_trace.slang
rem csTraceProbe hit branch, reads world-cache C). The only correct acceptance is to
rem dump irradAtlas_[0] directly (--probe-atlas-dump, bypassing gather) and compare
rem ON vs OFF. That is what this script asserts (check-s11-bounce-atlas.py).
rem
rem The dx12/vulkan backends are UNROLLED (no for %%B loop) on purpose: under
rem cmd //c launched from a POSIX shell, %%B is mangled to a literal %B, which
rem breaks both the viewer path and the screenshot/log filenames.
rem
rem Assertions (per backend):
rem   - --probe-cache-bounce re-shot exits 0, deviceErrors=0 (gate active, no GPU err)
rem   - atlas-dump ON/OFF exits 0, deviceErrors=0 (cross-frame UAV->SRV handoff ok)
rem   - check-s11-bounce-atlas.py onoff: NOT byte-identical AND mean diff >= MARGIN
rem     AND changed-pixel frac >= EPS (systematic, in-object-region rewrite)

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "MODEL=Suzanne-materials.nmesh"
set "SHOT=--mode probe --model %MODEL% --frames 64 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7"
set "MARGIN=0.2"
set "EPS=0.3"
set FAIL=0

rem ---- DX12 ----
echo [shot] dx12 ON  (--probe-cache-bounce)
"%ROOT%\build\DX12\viewer.exe" %SHOT% --probe-cache-bounce --screenshot "%OUT%verify-bounce-ON-dx12.png" > "%OUT%verify-on-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-on-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 ON device errors & set FAIL=1 ) else ( echo [OK] dx12 ON deviceErrors=0 )

echo [shot] dx12 OFF (control, no bounce)
"%ROOT%\build\DX12\viewer.exe" %SHOT% --screenshot "%OUT%verify-bounce-OFF-dx12.png" > "%OUT%verify-off-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-off-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 OFF device errors & set FAIL=1 ) else ( echo [OK] dx12 OFF deviceErrors=0 )

echo [dump] dx12 atlas ON  (--probe-cache-bounce --probe-atlas-dump)
"%ROOT%\build\DX12\viewer.exe" %SHOT% --probe-cache-bounce --probe-atlas-dump --screenshot "%OUT%verify-atlas-ON-dx12.png" > "%OUT%atlas-on-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 atlas ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 atlas ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%atlas-on-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 atlas ON device errors & set FAIL=1 ) else ( echo [OK] dx12 atlas ON deviceErrors=0 )

echo [dump] dx12 atlas OFF (control)
"%ROOT%\build\DX12\viewer.exe" %SHOT% --probe-atlas-dump --screenshot "%OUT%verify-atlas-OFF-dx12.png" > "%OUT%atlas-off-dx12.log" 2>&1
if errorlevel 1 ( echo [FAIL] dx12 atlas OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] dx12 atlas OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%atlas-off-dx12.log" <nul >nul
if errorlevel 1 ( echo [FAIL] dx12 atlas OFF device errors & set FAIL=1 ) else ( echo [OK] dx12 atlas OFF deviceErrors=0 )

echo [check] dx12 atlas ON vs OFF (M11.2 rewrite must be systematic, non-identical)
python "%ROOT%\tools\check-s11-bounce-atlas.py" onoff "%OUT%verify-atlas-ON-dx12.png" "%OUT%verify-atlas-OFF-dx12.png" %MARGIN% %EPS%
if errorlevel 1 ( echo [FAIL] dx12 atlas onoff & set FAIL=1 ) else ( echo [OK] dx12 atlas onoff )

rem ---- VULKAN ----
echo [shot] vulkan ON  (--probe-cache-bounce)
"%ROOT%\build\VULKAN\viewer.exe" %SHOT% --probe-cache-bounce --screenshot "%OUT%verify-bounce-ON-vulkan.png" > "%OUT%verify-on-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-on-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan ON device errors & set FAIL=1 ) else ( echo [OK] vulkan ON deviceErrors=0 )

echo [shot] vulkan OFF (control, no bounce)
"%ROOT%\build\VULKAN\viewer.exe" %SHOT% --screenshot "%OUT%verify-bounce-OFF-vulkan.png" > "%OUT%verify-off-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%verify-off-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan OFF device errors & set FAIL=1 ) else ( echo [OK] vulkan OFF deviceErrors=0 )

echo [dump] vulkan atlas ON  (--probe-cache-bounce --probe-atlas-dump)
"%ROOT%\build\VULKAN\viewer.exe" %SHOT% --probe-cache-bounce --probe-atlas-dump --screenshot "%OUT%verify-atlas-ON-vulkan.png" > "%OUT%atlas-on-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan atlas ON viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan atlas ON exit 0 )
findstr /C:"deviceErrors=0" "%OUT%atlas-on-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan atlas ON device errors & set FAIL=1 ) else ( echo [OK] vulkan atlas ON deviceErrors=0 )

echo [dump] vulkan atlas OFF (control)
"%ROOT%\build\VULKAN\viewer.exe" %SHOT% --probe-atlas-dump --screenshot "%OUT%verify-atlas-OFF-vulkan.png" > "%OUT%atlas-off-vulkan.log" 2>&1
if errorlevel 1 ( echo [FAIL] vulkan atlas OFF viewer exited nonzero & set FAIL=1 ) else ( echo [OK] vulkan atlas OFF exit 0 )
findstr /C:"deviceErrors=0" "%OUT%atlas-off-vulkan.log" <nul >nul
if errorlevel 1 ( echo [FAIL] vulkan atlas OFF device errors & set FAIL=1 ) else ( echo [OK] vulkan atlas OFF deviceErrors=0 )

echo [check] vulkan atlas ON vs OFF (M11.2 rewrite must be systematic, non-identical)
python "%ROOT%\tools\check-s11-bounce-atlas.py" onoff "%OUT%verify-atlas-ON-vulkan.png" "%OUT%verify-atlas-OFF-vulkan.png" %MARGIN% %EPS%
if errorlevel 1 ( echo [FAIL] vulkan atlas onoff & set FAIL=1 ) else ( echo [OK] vulkan atlas onoff )

if %FAIL%==0 ( echo [ALL GREEN] s11-bounce verify ) else ( echo [FAIL] s11-bounce verify )
exit /b %FAIL%

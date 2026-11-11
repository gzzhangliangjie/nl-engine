@echo off
setlocal
rem M-PT4 E1 rate range + E2 colorCurve acceptance shots + verify.
rem ASCII only. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12|VULKAN
rem
rem Evidence chain:
rem   - e1_rate_range.nfx: rate [60,120]/s min != max. Per-frame rate sampled
rem     from a dedicated pcg stream (rateSeq_, independent of the particle
rem     spawn stream); span=0 files are IEEE-identical to the old mean path
rem     (zero baseline drift). Steady anchor: alive=146 (mean rate 90/s x
rem     life 1.667s = 150 theory; range [100,200] hit).
rem   - e2_gradient.nfx: render.colorCurve 3 RGB keys (red->green->blue) +
rem     2 alpha keys, sampled on CPU when filling the instance stream (PS
rem     untouched). Steady anchor: alive=101 (rate 60/s fixed, CullingSprite).
rem   - Determinism: two identical DX12 e2 runs -> byte-identical PNGs
rem     (MD5 b5b747b8bc75febbd53d67e5dac688b4).
rem   - Negative: e_bad_colorcurve.nfx (empty alphaKeys) must be rejected with
rem     "--nfx: .nfx malformed colorCurve object (fallback to legacy fountain)".
rem     That run logs an error on purpose, only the log line is asserted.
rem   - Negative: default-off run must not print any nfx line.

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] particles-e1-rate-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --nfx "%ROOT%\assets\efx\e1_rate_range.nfx" --hud --screenshot "%OUT%particles-e1-rate-hud-%%B.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 640x720
  if errorlevel 1 set FAIL=1
  echo [shot] particles-e2-gradient-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --nfx "%ROOT%\assets\efx\e2_gradient.nfx" --hud --screenshot "%OUT%particles-e2-gradient-hud-%%B.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 640x720
  if errorlevel 1 set FAIL=1
)

echo [check] dx12 determinism: second identical e2 run must be byte-identical ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\e2_gradient.nfx" --hud --screenshot "%OUT%verify-determinism.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 640x720 > "%OUT%verify-run.log" 2>&1
fc /b "%OUT%particles-e2-gradient-hud-dx12.png" "%OUT%verify-determinism.png" >nul
if errorlevel 1 (
  echo [ERROR] determinism broken: baseline vs rerun differ
  set FAIL=1
) else (
  echo determinism byte-identical OK
)

echo [check] log anchors, dx12 ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\e1_rate_range.nfx" --hud --screenshot "%OUT%verify-log-dx12.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"nfx loaded: e1_rate_range.nfx" /C:"particles steady: alive=146 (nfx)"
if errorlevel 1 set FAIL=1
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\e2_gradient.nfx" --hud --screenshot "%OUT%verify-log2-dx12.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"particles steady: alive=101 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] log anchors, vulkan ...
"%ROOT%\build\vulkan\viewer.exe" --nfx "%ROOT%\assets\efx\e1_rate_range.nfx" --hud --screenshot "%OUT%verify-log-vulkan.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"particles steady: alive=146 (nfx)"
if errorlevel 1 set FAIL=1
"%ROOT%\build\vulkan\viewer.exe" --nfx "%ROOT%\assets\efx\e2_gradient.nfx" --hud --screenshot "%OUT%verify-log2-vulkan.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"particles steady: alive=101 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] negative: malformed colorCurve must be rejected ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\e_bad_colorcurve.nfx" --screenshot "%OUT%verify-bad-nfx.png" --frames 8 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"--nfx: .nfx malformed colorCurve object (fallback to legacy fountain)"
if errorlevel 1 (
  echo [ERROR] malformed colorCurve was not rejected
  set FAIL=1
) else (
  echo negative OK: malformed colorCurve rejected with fallback log
)

echo [check] negative: default-off run must NOT print nfx lines ...
"%ROOT%\build\dx12\viewer.exe" --hud --screenshot "%OUT%verify-off-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"nfx" >nul
if errorlevel 1 (
  echo negative OK: no nfx line without --nfx
) else (
  echo [ERROR] unexpected nfx line in default-off run
  set FAIL=1
)

if %FAIL%==1 (
  echo [FAIL] particles-pt4 verify
  exit /b 1
)
echo [OK] particles-pt4 verify: e1/e2 pair x2 backends + determinism + anchors + negatives
exit /b 0

@echo off
setlocal
rem M-PT5 E5 per-particle rotation + R1 per-particle sizeY acceptance shots + verify.
rem ASCII only. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12|VULKAN
rem
rem Evidence chain:
rem   - Instance stream 48->64B (float4 x4): progressMisc.z = angle (radians),
rem     progressMisc.w = per-particle Y/X ratio, misc2 = spare (E3/E4/E8).
rem   - Zero-drift proof: old rand slots (s*5u+0..4) untouched, new slots derive
rem     from s*7u+0x100u+{0,1,2} (disjoint input space); angle=0 -> cos=1/sin=0
rem     IEEE identity; w=ratioY same float as old CB scalar g_params.z.
rem     pt0..pt3 baselines re-verified BYTE-IDENTICAL (22/22, no reshot needed).
rem   - e5_spin.nfx: ArrowNE.png (asymmetric probe texture), ang [0, 2pi],
rem     angVel [1.5, 4.0] rad/s. Steady anchor: alive=51 (rate 30/s x life
rem     1.667s = 50 theory). Arrows visibly point in random directions.
rem   - r1_sizey.nfx: sizeYRange [0.25, 1.6] m on soft discs. Steady anchor:
rem     alive=51. Discs visibly squashed/stretched per particle.
rem   - Determinism: two identical DX12 e5 runs -> byte-identical PNGs
rem     (MD5 01abd003841ad613bfb22893ee753089).
rem   - Negative runs intentionally log an error line (errorCount=1 -> viewer
rem     acceptance line fails); only the log line is asserted, exit code is not.

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] particles-e5-spin-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --nfx "%ROOT%\assets\efx\e5_spin.nfx" --hud --screenshot "%OUT%particles-e5-spin-hud-%%B.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 640x720
  if errorlevel 1 set FAIL=1
  echo [shot] particles-r1-sizey-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --nfx "%ROOT%\assets\efx\r1_sizey.nfx" --hud --screenshot "%OUT%particles-r1-sizey-hud-%%B.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 640x720
  if errorlevel 1 set FAIL=1
)

echo [check] dx12 determinism: second identical e5 run must be byte-identical ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\e5_spin.nfx" --hud --screenshot "%OUT%verify-determinism.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 640x720 > "%OUT%verify-run.log" 2>&1
fc /b "%OUT%particles-e5-spin-hud-dx12.png" "%OUT%verify-determinism.png" >nul
if errorlevel 1 (
  echo [ERROR] determinism broken: baseline vs rerun differ
  set FAIL=1
) else (
  echo determinism byte-identical OK
)

echo [check] log anchors, dx12 ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\e5_spin.nfx" --screenshot "%OUT%verify-log1-dx12.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"particles steady: alive=51 (nfx)"
if errorlevel 1 set FAIL=1
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\r1_sizey.nfx" --screenshot "%OUT%verify-log2-dx12.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"particles steady: alive=51 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] log anchors, vulkan ...
"%ROOT%\build\vulkan\viewer.exe" --nfx "%ROOT%\assets\efx\e5_spin.nfx" --screenshot "%OUT%verify-log1-vulkan.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"particles steady: alive=51 (nfx)"
if errorlevel 1 set FAIL=1
"%ROOT%\build\vulkan\viewer.exe" --nfx "%ROOT%\assets\efx\r1_sizey.nfx" --screenshot "%OUT%verify-log2-vulkan.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"particles steady: alive=51 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] negative: malformed ang array must be rejected ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\e_bad_ang.nfx" --screenshot "%OUT%verify-bad-ang.png" --frames 8 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"--nfx: .nfx malformed ang/angVel (want [min,max] radian pairs) (fallback to legacy fountain)"
if errorlevel 1 (
  echo [ERROR] malformed ang was not rejected
  set FAIL=1
) else (
  echo negative OK: malformed ang rejected with fallback log
)

echo [check] negative: malformed sizeYRange must be rejected ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\e_bad_sizey.nfx" --screenshot "%OUT%verify-bad-sizey.png" --frames 8 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"--nfx: .nfx malformed sizeYRange (want [min,max] meter pair) (fallback to legacy fountain)"
if errorlevel 1 (
  echo [ERROR] malformed sizeYRange was not rejected
  set FAIL=1
) else (
  echo negative OK: malformed sizeYRange rejected with fallback log
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
  echo [FAIL] particles-pt5 verify
  exit /b 1
)
echo [OK] particles-pt5 verify: e5/r1 pair x2 backends + determinism + anchors + negatives
exit /b 0

@echo off
setlocal
rem M-PT1 .nfx v1 closed loop acceptance shots + verify.
rem ASCII only. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12|VULKAN
rem
rem Evidence chain:
rem   - vfx-tools convert CullingSprite.efkefc -> CullingSprite.nfx (exit 0);
rem     GpuParticles_emit_line.efkefc -> exit 2 with reason list (subset gate).
rem   - viewer --nfx loads the converted asset (log anchor "nfx loaded:") and
rem     simulates it deterministically: 120 frames -> alive=101 (rate 60/s,
rem     life 1.67s, CullingSprite params). Red billboard glow behind Suzanne.
rem   - Determinism: two identical DX12 runs produce byte-identical PNGs.
rem   - Negative: without --nfx no nfx line appears (default-off untouched).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

echo [tool] vfx-tools selftest ...
"%ROOT%\build\dx12\vfx-tools.exe" selftest
if errorlevel 1 set FAIL=1

echo [tool] convert positive: CullingSprite.efkefc (expect exit 0) ...
"%ROOT%\build\dx12\vfx-tools.exe" convert "%ROOT%\assets\efx\CullingSprite.efkefc" "%OUT%nfx-roundtrip.nfx" >nul 2>&1
if errorlevel 1 (
  echo [ERROR] convert CullingSprite failed
  set FAIL=1
) else (
  echo convert positive OK
)

echo [tool] convert negative: GpuParticles_emit_line.efkefc (expect exit 2) ...
"%ROOT%\build\dx12\vfx-tools.exe" convert "%ROOT%\assets\efx\GpuParticles_emit_line.efkefc" "%OUT%nfx-negative.nfx" 2>&1 | findstr /C:"exit 2: unsupported modules" >nul
if errorlevel 1 (
  echo [ERROR] expected exit-2 unsupported-modules report
  set FAIL=1
) else (
  echo convert negative OK
)

for %%B in (dx12 vulkan) do (
  echo [shot] particles-nfx-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --nfx "%ROOT%\assets\efx\CullingSprite.nfx" --hud --screenshot "%OUT%particles-nfx-hud-%%B.png" --frames 120 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 set FAIL=1
)

echo [check] dx12 determinism: second identical run must be byte-identical ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\CullingSprite.nfx" --hud --screenshot "%OUT%verify-determinism.png" --frames 120 --yaw 12 --pitch 9 --dist 4.2 --size 640x720 > "%OUT%verify-run.log" 2>&1
fc /b "%OUT%particles-nfx-hud-dx12.png" "%OUT%verify-determinism.png" >nul
if errorlevel 1 (
  echo [ERROR] determinism broken: baseline vs rerun differ
  set FAIL=1
) else (
  echo determinism byte-identical OK
)

echo [check] loaded + steady log anchor, dx12 ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\CullingSprite.nfx" --hud --screenshot "%OUT%verify-log-dx12.png" --frames 120 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 2>&1 | findstr /C:"nfx loaded: CullingSprite.nfx" /C:"particles steady: alive=101 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] loaded + steady log anchor, vulkan ...
"%ROOT%\build\vulkan\viewer.exe" --nfx "%ROOT%\assets\efx\CullingSprite.nfx" --hud --screenshot "%OUT%verify-log-vulkan.png" --frames 120 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 2>&1 | findstr /C:"nfx loaded: CullingSprite.nfx" /C:"particles steady: alive=101 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] negative: default-off run must NOT print nfx anchors ...
"%ROOT%\build\dx12\viewer.exe" --hud --screenshot "%OUT%verify-off-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 2>&1 | findstr /C:"nfx loaded" >nul
if errorlevel 1 (
  echo negative OK: no nfx line without --nfx
) else (
  echo [ERROR] unexpected nfx loaded line in default-off run
  set FAIL=1
)

if %FAIL%==1 (
  echo [FAIL] particles-pt1 verify
  exit /b 1
)
echo [OK] particles-pt1 verify: tool positive/negative + hud pair + determinism + anchors + negative
exit /b 0

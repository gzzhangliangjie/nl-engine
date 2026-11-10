@echo off
setlocal
rem M-PT2 texture sampling + sizeY acceptance shots + verify.
rem ASCII only. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12|VULKAN
rem
rem Evidence chain:
rem   - tex_particle01.nfx  : texture mode, alpha blend, square (0.4, 0.4).
rem     Particle01.png 128x128 (Effekseer 1.80.8 Sample 00_Basic, CC-0).
rem   - tex_flame01_rect.nfx: texture + non-square (0.5 x 0.25, ratioY 0.5 via
rem     CB g_params.z) + additive pipeline (M-PT1 lazy-built variant).
rem   - viewer --nfx loads them; log anchor "nfx texture: <file> (WxH)" is
rem     printed by Particles::ensureTexture on first draw. 120 frames ->
rem     alive=101 (rate 60/s, life 1.67s steady state).
rem   - Determinism: two identical DX12 texture-mode runs produce
rem     byte-identical PNGs (texture upload happens inside the frame cmd
rem     list every run, no timing leakage into the screenshot).
rem   - Soft-disc fallback verified manually: texture file missing ->
rem     log "nfx texture not found ... (soft-disc fallback)", frames keep
rem     rendering (deviceErrors=1 -> acceptance fail is the expected gate).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] particles-tex-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --nfx "%ROOT%\assets\efx\tex_particle01.nfx" --hud --screenshot "%OUT%particles-tex-hud-%%B.png" --frames 120 --yaw 12 --pitch 9 --dist 3.2 --size 640x720
  if errorlevel 1 set FAIL=1
  echo [shot] particles-tex-flame on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --nfx "%ROOT%\assets\efx\tex_flame01_rect.nfx" --hud --screenshot "%OUT%particles-tex-flame-%%B.png" --frames 120 --yaw 12 --pitch 9 --dist 3.2 --size 640x720
  if errorlevel 1 set FAIL=1
)

echo [check] dx12 determinism: second identical texture-mode run must be byte-identical ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\tex_particle01.nfx" --hud --screenshot "%OUT%verify-determinism.png" --frames 120 --yaw 12 --pitch 9 --dist 3.2 --size 640x720 > "%OUT%verify-run.log" 2>&1
fc /b "%OUT%particles-tex-hud-dx12.png" "%OUT%verify-determinism.png" >nul
if errorlevel 1 (
  echo [ERROR] determinism broken: baseline vs rerun differ
  set FAIL=1
) else (
  echo determinism byte-identical OK
)

echo [check] texture + steady log anchors, dx12 ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\tex_particle01.nfx" --hud --screenshot "%OUT%verify-log-dx12.png" --frames 120 --yaw 12 --pitch 9 --dist 3.2 --size 320x240 2>&1 | findstr /C:"nfx texture: Particle01.png (128x128)" /C:"particles steady: alive=101 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] texture + steady log anchors, vulkan ...
"%ROOT%\build\vulkan\viewer.exe" --nfx "%ROOT%\assets\efx\tex_particle01.nfx" --hud --screenshot "%OUT%verify-log-vulkan.png" --frames 120 --yaw 12 --pitch 9 --dist 3.2 --size 320x240 2>&1 | findstr /C:"nfx texture: Particle01.png (128x128)" /C:"particles steady: alive=101 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] negative: default-off run must NOT print texture anchors ...
"%ROOT%\build\dx12\viewer.exe" --hud --screenshot "%OUT%verify-off-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 3.2 --size 320x240 2>&1 | findstr /C:"nfx texture" >nul
if errorlevel 1 (
  echo negative OK: no texture line without --nfx
) else (
  echo [ERROR] unexpected nfx texture line in default-off run
  set FAIL=1
)

if %FAIL%==1 (
  echo [FAIL] particles-pt2 verify
  exit /b 1
)
echo [OK] particles-pt2 verify: hud/flame pair x2 backends + determinism + texture anchors + negative
exit /b 0

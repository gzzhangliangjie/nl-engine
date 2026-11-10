@echo off
setlocal
rem M-PT3 texture mip chain + flipbook acceptance shots + verify.
rem ASCII only. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12|VULKAN
rem
rem Evidence chain:
rem   - tex_flipbook.nfx: flipbook 4x2 grid (frameLength 12 Efke frames = 0.2s,
rem     loop, cell 0.25x0.5), velocity spread so particles of different ages
rem     land at different positions -> multiple frames visible at once.
rem     flipbook_4x2.png 256x128 PIL-generated (colored circles + digits 0-7).
rem   - Mip chain: ensureTexture CPU box-filter downsampling, full mipLevels
rem     texture, writeTexture per-layer upload; sampler mipFilter linear.
rem   - Flipbook: frame numbers folded on CPU (Effekseer GetUV-style integer
rem     math), VS emits flipUv via branchless unified formula
rem     uvRect.xy + uvRect.zw*(frameXY + localUv); non-flipbook paths are
rem     IEEE-identical to M-PT2 (uvRect = 0,0,1,1).
rem   - Log anchor "nfx texture: flipbook_4x2.png (256x128)" printed by
rem     ensureTexture on first draw. 120 frames -> alive=101 steady state.
rem   - Determinism: two identical DX12 flipbook runs -> byte-identical PNGs.

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] particles-flipbook-hud on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --nfx "%ROOT%\assets\efx\tex_flipbook.nfx" --hud --screenshot "%OUT%particles-flipbook-hud-%%B.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 640x720
  if errorlevel 1 set FAIL=1
)

echo [check] dx12 determinism: second identical flipbook run must be byte-identical ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\tex_flipbook.nfx" --hud --screenshot "%OUT%verify-determinism.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 640x720 > "%OUT%verify-run.log" 2>&1
fc /b "%OUT%particles-flipbook-hud-dx12.png" "%OUT%verify-determinism.png" >nul
if errorlevel 1 (
  echo [ERROR] determinism broken: baseline vs rerun differ
  set FAIL=1
) else (
  echo determinism byte-identical OK
)

echo [check] texture + steady log anchors, dx12 ...
"%ROOT%\build\dx12\viewer.exe" --nfx "%ROOT%\assets\efx\tex_flipbook.nfx" --hud --screenshot "%OUT%verify-log-dx12.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"nfx texture: flipbook_4x2.png (256x128)" /C:"particles steady: alive=101 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] texture + steady log anchors, vulkan ...
"%ROOT%\build\vulkan\viewer.exe" --nfx "%ROOT%\assets\efx\tex_flipbook.nfx" --hud --screenshot "%OUT%verify-log-vulkan.png" --frames 120 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"nfx texture: flipbook_4x2.png (256x128)" /C:"particles steady: alive=101 (nfx)"
if errorlevel 1 set FAIL=1

echo [check] negative: default-off run must NOT print texture anchors ...
"%ROOT%\build\dx12\viewer.exe" --hud --screenshot "%OUT%verify-off-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 3.6 --size 320x240 2>&1 | findstr /C:"nfx texture" >nul
if errorlevel 1 (
  echo negative OK: no texture line without --nfx
) else (
  echo [ERROR] unexpected nfx texture line in default-off run
  set FAIL=1
)

if %FAIL%==1 (
  echo [FAIL] particles-pt3 verify
  exit /b 1
)
echo [OK] particles-pt3 verify: flipbook pair x2 backends + determinism + texture anchors + negative
exit /b 0

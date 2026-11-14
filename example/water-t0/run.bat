@echo off
setlocal
rem M-WAT0 / water track - SSR screen-space reflection acceptance shots.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M-WAT0 adds --ssr to --mode pulling (matresolve into hdrSceneColor_):
rem   - march chain = radial-distance MIN pyramid from vis deviceZ + scene color
rem     mean chain + UE SSRT-style march + additive compose before tonemap
rem   - --ssr off (default) must be byte-identical to the frozen pulling baseline
rem     (zero-drift gate: same args minus --ssr vs frozen ssr-off png)
rem   - --ssr on must CHANGE the frame (A/B differ, reflections visible on glossy
rem     surfaces; background untouched)
rem   - deterministic: same args re-shot = byte-identical per backend
rem   - log anchor: "ssr: march chain ready" present when on, absent when off
rem   - dx12 vs vulkan agree within tolerance (check-ssr.py near)
rem   - negative: unknown flag exits nonzero (usage)

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--mode pulling --model Suzanne-materials.nmesh --shading 5 --frames 4 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7"
set FAIL=0

echo [shot] re-shoot ssr-on on both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --ssr --screenshot "%OUT%verify-ssr-on-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] ssr-on-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [shot] re-shoot ssr-off baseline on both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --screenshot "%OUT%verify-ssr-off-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] ssr-off-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] bytes: ssr-on re-shot vs frozen M-WAT0 baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%ssr-on-%%B.png" "%OUT%verify-ssr-on-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] ssr-on-%%B bytes differ from baseline
    set FAIL=1
  )
)

echo [check] bytes: ssr-off re-shot vs frozen zero-drift baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%ssr-off-%%B.png" "%OUT%verify-ssr-off-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] ssr-off-%%B bytes differ from baseline
    set FAIL=1
  )
)

echo [check] A/B: ssr must change the frame
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-ssr-off-%%B.png" "%OUT%verify-ssr-on-%%B.png" >nul
  if not errorlevel 1 (
    echo [FAIL] ssr-%%B on/off identical - reflections not applied
    set FAIL=1
  )
)

echo [check] deterministic: ssr-on re-shot twice byte-identical
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --ssr --screenshot "%OUT%verify-ssr-det-%%B.png" >nul
  fc /b "%OUT%verify-ssr-on-%%B.png" "%OUT%verify-ssr-det-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] ssr-%%B not deterministic
    set FAIL=1
  )
)

echo [check] log anchors
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --ssr --frames 1 --screenshot "%OUT%verify-anchor-%%B.png" > "%OUT%verify-anchor-%%B.log" 2>&1
  findstr /c:"ssr: march chain ready" "%OUT%verify-anchor-%%B.log" >nul
  if errorlevel 1 (
    echo [FAIL] ssr-%%B log anchor missing
    set FAIL=1
  )
  findstr /c:"deviceErrors=0" "%OUT%verify-anchor-%%B.log" >nul
  if errorlevel 1 (
    echo [FAIL] ssr-%%B deviceErrors nonzero
    set FAIL=1
  )
)

echo [check] cross-backend near tolerance
python "%ROOT%\tools\check-ssr.py" near "%OUT%verify-ssr-on-dx12.png" "%OUT%verify-ssr-on-vulkan.png"
if errorlevel 1 (
  echo [FAIL] dx12 vs vulkan ssr-on beyond near tolerance
  set FAIL=1
)

echo [negative] invalid mode exits nonzero
"%ROOT%\build\dx12\viewer.exe" --mode nonexistent-mode >nul 2>&1
if not errorlevel 1 (
  echo [FAIL] invalid mode accepted
  set FAIL=1
)

if %FAIL%==0 (
  echo ALL GREEN
  exit /b 0
)
exit /b 1

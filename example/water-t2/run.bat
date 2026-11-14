@echo off
setlocal
rem M-WAT2 / water track - refraction + absorption + fresnel + foam composite.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M-WAT2 adds csWaterComposite (Q-WA7=A, UE-style deferred water composite):
rem   - water pixels become black placeholders in psMatResolve / csDirect
rem   - csWaterCopy snapshots hdrSceneColor_ into waterBg_ (scene without water)
rem   - csWaterComposite re-solves water pixels: bilateral thickness + Snell UV
rem     distortion + Beer-Lambert absorption + Fresnel-mixed SSR + sun specular
rem     + crest foam; --water implies the SSR chain (fp.ssr = o.ssr || o.water)
rem   - --water off (default) must stay byte-identical to the water-t0 ssr-on
rem     frozen baseline (zero-drift gate)
rem   - reflection evidence: full chain vs --water-noreflect must differ (the
rem     whole-track hard gate: water without reflections looks unchanged = FAIL)
rem   - refraction evidence: full chain vs --water-norefract must differ (the
rem     refracted pool floor disappears = in-scatter only, absorption stays)
rem   - deterministic: same args re-shot = byte-identical per backend
rem   - log anchors: water: gerstner grid ready + water: composite ready
rem   - dx12 vs vulkan agree within tolerance (check-ssr.py near)
rem
set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--mode pulling --model Suzanne-materials.nmesh --shading 5 --ssr --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7"
set FAIL=0

echo [shot] full water chain on both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --water --frames 4 --screenshot "%OUT%verify-water-full-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-full-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [shot] reflection evidence A/B: --water-noreflect must change the frame
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --water --water-noreflect --frames 4 --screenshot "%OUT%verify-water-noreflect-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-noreflect-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [shot] refraction evidence A/B: --water-norefract must change the frame
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --water --water-norefract --frames 4 --screenshot "%OUT%verify-water-norefract-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-norefract-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [shot] zero-drift gate: --ssr only, --water off
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --frames 4 --screenshot "%OUT%verify-water-off-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-off-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [shot] later frame for wave motion check
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --water --frames 12 --screenshot "%OUT%verify-water-late-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-late-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] bytes: water-off re-shot vs frozen water-t0 ssr-on baseline
for %%B in (dx12 vulkan) do (
  fc /b "%ROOT%\example\water-t0\ssr-on-%%B.png" "%OUT%verify-water-off-%%B.png" >nul 2>nul
  if errorlevel 1 (
    echo [FAIL] water-off-%%B bytes differ from water-t0 ssr-on baseline
    set FAIL=1
  )
)

echo [check] A/B: full chain vs noreflect must differ - reflection evidence
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-water-full-%%B.png" "%OUT%verify-water-noreflect-%%B.png" >nul
  if not errorlevel 1 (
    echo [FAIL] water-%%B full vs noreflect identical - reflection missing
    set FAIL=1
  )
)

echo [check] A/B: full chain vs norefract must differ - refraction evidence
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-water-full-%%B.png" "%OUT%verify-water-norefract-%%B.png" >nul
  if not errorlevel 1 (
    echo [FAIL] water-%%B full vs norefract identical - refraction missing
    set FAIL=1
  )
)

echo [check] A/B: water on must differ from water off
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-water-off-%%B.png" "%OUT%verify-water-full-%%B.png" >nul
  if not errorlevel 1 (
    echo [FAIL] water-%%B on/off identical
    set FAIL=1
  )
)

echo [check] wave motion: frames 4 vs frames 12 must differ
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-water-full-%%B.png" "%OUT%verify-water-late-%%B.png" >nul
  if not errorlevel 1 (
    echo [FAIL] water-%%B wave motion missing
    set FAIL=1
  )
)

echo [check] deterministic: full-chain re-shot twice byte-identical
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --water --frames 4 --screenshot "%OUT%verify-water-det-%%B.png" >nul
  fc /b "%OUT%verify-water-full-%%B.png" "%OUT%verify-water-det-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-%%B not deterministic
    set FAIL=1
  )
)

echo [check] log anchors
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --water --frames 1 --screenshot "%OUT%verify-anchor-%%B.png" > "%OUT%verify-anchor-%%B.log" 2>&1
  findstr /c:"water: gerstner grid ready" "%OUT%verify-anchor-%%B.log" >nul
  if errorlevel 1 (
    echo [FAIL] water-%%B gerstner anchor missing
    set FAIL=1
  )
  findstr /c:"water: composite ready" "%OUT%verify-anchor-%%B.log" >nul
  if errorlevel 1 (
    echo [FAIL] water-%%B composite anchor missing
    set FAIL=1
  )
  findstr /c:"deviceErrors=0" "%OUT%verify-anchor-%%B.log" >nul
  if errorlevel 1 (
    echo [FAIL] water-%%B deviceErrors nonzero
    set FAIL=1
  )
)

echo [check] cross-backend near tolerance
python "%ROOT%\tools\check-ssr.py" near "%OUT%verify-water-full-dx12.png" "%OUT%verify-water-full-vulkan.png"
if errorlevel 1 (
  echo [FAIL] dx12 vs vulkan water-full beyond near tolerance
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

@echo off
setlocal
rem M-WAT1 / water track - Gerstner wave + analytic normal acceptance shots.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M-WAT1 adds --water to --mode pulling (water pseudo-cluster into vis buffer):
rem   - 64x64 quad grid, 64 pseudo-clusters, materialId = 0xFFFF reserved marker
rem   - VS displaces vertices (real depth); lighting re-computes Gerstner +
rem     analytic normal per-pixel (same waterGerstner single source)
rem   - --water off (default) must be byte-identical to frozen M-WAT0 ssr-on
rem     baseline (zero-drift gate: same args minus --water)
rem   - --water on must CHANGE the frame (A/B differ, waves + specular visible)
rem   - waves must MOVE: frames 4 vs frames 8 = different waterTime = differ
rem   - deterministic: same args re-shot = byte-identical per backend
rem   - log anchor: "water: gerstner grid ready" present when on, absent when off
rem   - dx12 vs vulkan agree within tolerance (check-ssr.py near)
rem   - negative: invalid mode exits nonzero (usage)

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--mode pulling --model Suzanne-materials.nmesh --shading 5 --ssr --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7"
set FAIL=0

echo [shot] re-shoot water-on on both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --water --frames 4 --screenshot "%OUT%verify-water-on-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-on-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [shot] re-shoot water-off baseline on both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --frames 4 --screenshot "%OUT%verify-water-off-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-off-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [shot] water-on later frame for wave-motion check
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --water --frames 12 --screenshot "%OUT%verify-water-late-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-late-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] bytes: water-off re-shot vs frozen M-WAT0 ssr-on baseline
for %%B in (dx12 vulkan) do (
  fc /b "%ROOT%\example\water-t0\ssr-on-%%B.png" "%OUT%verify-water-off-%%B.png" >nul 2>nul
  if errorlevel 1 (
    echo [FAIL] water-off-%%B bytes differ from water-t0 ssr-on baseline
    set FAIL=1
  )
)

echo [check] bytes: water-on re-shot vs frozen M-WAT1 baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%water-on-%%B.png" "%OUT%verify-water-on-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] water-on-%%B bytes differ from baseline
    set FAIL=1
  )
)

echo [check] A/B: water must change the frame
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-water-off-%%B.png" "%OUT%verify-water-on-%%B.png" >nul
  if not errorlevel 1 (
    echo [FAIL] water-%%B on/off identical - waves not applied
    set FAIL=1
  )
)

echo [check] wave motion: frames 4 vs frames 12 must differ
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-water-on-%%B.png" "%OUT%verify-water-late-%%B.png" >nul
  if not errorlevel 1 (
    echo [FAIL] water-%%B wave motion missing - time not advancing
    set FAIL=1
  )
)

echo [check] deterministic: water-on re-shot twice byte-identical
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" %ARGS% --water --frames 4 --screenshot "%OUT%verify-water-det-%%B.png" >nul
  fc /b "%OUT%verify-water-on-%%B.png" "%OUT%verify-water-det-%%B.png" >nul
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
    echo [FAIL] water-%%B log anchor missing
    set FAIL=1
  )
  findstr /c:"deviceErrors=0" "%OUT%verify-anchor-%%B.log" >nul
  if errorlevel 1 (
    echo [FAIL] water-%%B deviceErrors nonzero
    set FAIL=1
  )
)

echo [check] cross-backend near tolerance
python "%ROOT%\tools\check-ssr.py" near "%OUT%verify-water-on-dx12.png" "%OUT%verify-water-on-vulkan.png"
if errorlevel 1 (
  echo [FAIL] dx12 vs vulkan water-on beyond near tolerance
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

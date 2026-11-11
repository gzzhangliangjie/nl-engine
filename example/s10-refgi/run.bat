@echo off
setlocal
rem M10.3 / S10 - reference-gi baseline + --compare side-by-side acceptance.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M10.3 delivers (Q14-1..Q14-4, all option A):
rem   --mode reference-gi : progressive multi-frame GI reference (one cosine-
rem     weighted hemisphere bounce + skyGradient on miss + shadow-ray direct
rem     light, same conventions as the probe chain: 104-tri coarse proxy,
rem     dual albedo, no NdotL degrade). Q3 errata: S9 reference is direct-light
rem     only and cannot serve as a GI ruler; S10 mints its own.
rem   viewer --compare <png> : right-half overlay of the reference image
rem     (viewport-scissored, PSO depth disabled, linear clamp sampling).
rem Assertions:
rem   - reference-gi re-shot on both backends exits 0, deviceErrors=0
rem   - re-shot == frozen baseline (bytes; 1024 spp deterministic accumulation)
rem   - log anchor: reference-gi: 1024 / 1024 spp (100%)
rem   - trend gate: probe gather baseline vs GI reference tile-luma pearson
rem     (Q3 errata: trend ruler, not per-pixel judge; measured 0.960 / MAD 3.4)
rem   - --compare overlay smoke: exit 0, deviceErrors=0, right half matches
rem     reference mean (python spot check)
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
rem 1024 spp = 16 frames x fixed 64 spp/frame (viewer has no per-run spp knob).
set "ARGS=--model Suzanne-materials.nmesh --size 960x540 --yaw 12 --pitch 9 --dist 7 --frames 16 --no-hud"
set FAIL=0

echo [shot] re-shoot reference-gi on both backends (dx12 log captured to verify-log.txt)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode reference-gi %ARGS% --screenshot "%OUT%verify-refgi-%%B.png" > "%OUT%verify-log.txt" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode reference-gi %ARGS% --screenshot "%OUT%verify-refgi-%%B.png" >nul
  )
  if errorlevel 1 (
    echo [FAIL] reference-gi-%%B viewer exited nonzero
    set FAIL=1
  )
)

findstr /C:"deviceErrors=0" "%OUT%verify-log.txt" <nul >nul
if errorlevel 1 (
  echo [FAIL] dx12 re-shot reported device errors
  set FAIL=1
) else (
  echo [OK] dx12 re-shot deviceErrors=0
)

echo [check] bytes: re-shot vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%refgi-%%B.png" "%OUT%verify-refgi-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] reference-gi-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] reference-gi-%%B bytes identical to baseline
  )
)

echo [check] log anchor: reference-gi accumulation complete
rem anchor avoids literal percent: cmd eats a single % inside the pattern
rem (100%% needed in bat). findstr gets <nul so a broken file arg can never
rem make it block on stdin (verified hang without it).
findstr /C:"reference-gi: 1024 / 1024 spp" "%OUT%verify-log.txt" <nul >nul
if errorlevel 1 (
  echo [FAIL] reference-gi accumulation anchor missing
  set FAIL=1
) else (
  echo [OK] reference-gi 1024/1024 spp anchor present
)

echo [check] trend gate: gather baseline vs GI reference (pearson + MAD)
python "%ROOT%\tools\check-s10-refgi.py" trend "%ROOT%\example\s10-probe\probe-dx12.png" "%OUT%refgi-dx12.png" 0.60 30.0
if errorlevel 1 (
  echo [FAIL] dx12 trend gate failed
  set FAIL=1
)
python "%ROOT%\tools\check-s10-refgi.py" trend "%ROOT%\example\s10-probe\probe-vulkan.png" "%OUT%refgi-vulkan.png" 0.60 30.0
if errorlevel 1 (
  echo [FAIL] vulkan trend gate failed
  set FAIL=1
)

echo [check] --compare overlay smoke on dx12 (probe mode, right half = reference)
"%ROOT%\build\dx12\viewer.exe" --mode probe --model Suzanne-materials.nmesh --size 960x540 --yaw 12 --pitch 9 --dist 7 --frames 3 --compare "%OUT%refgi-dx12.png" --screenshot "%OUT%verify-compare.png" > "%OUT%verify-compare-log.txt" 2>&1
if errorlevel 1 (
  echo [FAIL] compare smoke viewer exited nonzero
  set FAIL=1
)
findstr /C:"compare: loaded" "%OUT%verify-compare-log.txt" <nul >nul
if errorlevel 1 (
  echo [FAIL] compare load anchor missing
  set FAIL=1
) else (
  echo [OK] compare: loaded anchor present
)
python "%ROOT%\tools\check-s10-refgi.py" righthalf "%OUT%verify-compare.png" "%OUT%refgi-dx12.png" 0.80 60.0
if errorlevel 1 (
  echo [FAIL] compare overlay right-half trend check failed
  set FAIL=1
) else (
  echo [OK] compare overlay right half tracks reference
)

echo [negative] missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode reference-gi --model missing.nmesh --frames 1 --no-hud --screenshot "%OUT%verify-negative.png" >nul
if errorlevel 1 (
  echo [OK] negative: missing model exits nonzero
) else (
  echo [FAIL] negative: missing model exited zero
  set FAIL=1
)

if "%FAIL%"=="1" (
  echo VERIFY FAILED
  exit /b 1
)
echo VERIFY OK
exit /b 0

@echo off
setlocal
rem M9.1 / S9 - shadow-ray (1 ray/pixel) acceptance shots.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M9.1 changed --mode shadow semantics: it now casts 1 shadow ray per
rem pixel via the M8 TLAS (S3 fallback coarse proxy, 104 tri). So shadow is
rem NO LONGER byte-identical to pulling (M9.0 asserted that; this milestone
rem breaks it by design). What we assert instead:
rem   - both backends render the shadow mode exit 0
rem   - re-shot == frozen M9.1 baseline (deterministic, no regression)
rem   - log anchor: shadow scene ready AND 1 shadow ray/pixel via TLAS present
rem     but TLAS not ready must be ABSENT (rays actually dispatched)
rem   - shadow rays only ATTENUATE: M9.1 vs M9.0 no-ray baseline has 0 brighter
rem     pixels (shadows darken, never brighten) -- tool check-s9shadow.py darken
rem   - dx12 vs vulkan agree within tolerance -- tool check-s9shadow.py near
rem   - negative: missing model exits nonzero
rem   - geometry-diff.png (raster fine LOD vs ray proxy) is present

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --shading 5 --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 9"
set FAIL=0

echo [shot] re-shoot shadow on both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode shadow %ARGS% --screenshot "%OUT%verify-shadow-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] shadow-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] bytes: re-shot vs frozen M9.1 baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%shadow-%%B.png" "%OUT%verify-shadow-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] shadow-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] shadow-%%B bytes identical to baseline
  )
)

echo [check] log anchor: rays dispatched via TLAS
"%ROOT%\build\dx12\viewer.exe" --mode shadow --model Suzanne-materials.nmesh --shading 5 --screenshot "%OUT%verify-log-dx12.png" --frames 8 --no-hud --size 320x240 > "%OUT%verify-log.txt" 2>&1
findstr /C:"shadow scene ready" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] shadow scene ready log line missing
  set FAIL=1
) else (
  echo [OK] shadow scene ready log line present
)
findstr /C:"1 shadow ray/pixel via TLAS" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] rays not dispatched - expected 1 shadow ray per pixel via TLAS
  set FAIL=1
) else (
  echo [OK] rays dispatched via TLAS
)
findstr /C:"TLAS not ready" "%OUT%verify-log.txt" >nul
if not errorlevel 1 (
  echo [FAIL] TLAS not ready - shadow rays disabled
  set FAIL=1
) else (
  echo [OK] TLAS ready - no TLAS not ready line
)

echo [check] shadow rays only attenuate: M9.1 vs M9.0 no-ray baseline
python "%ROOT%\tools\check-s9shadow.py" darken "%OUT%verify-shadow-dx12.png" "%ROOT%\example\s9-gbuffer\shadow-dx12.png"
if errorlevel 1 (
  echo [FAIL] only-darken gate failed
  set FAIL=1
) else (
  echo [OK] only-darken gate passed
)

echo [check] dx12 vs vulkan within tolerance
python "%ROOT%\tools\check-s9shadow.py" near "%OUT%verify-shadow-dx12.png" "%OUT%verify-shadow-vulkan.png" 0.01
if errorlevel 1 (
  echo [FAIL] dx12/vulkan near gate failed
  set FAIL=1
) else (
  echo [OK] dx12/vulkan near gate passed
)

echo [check] geometry-diff.png present
if not exist "%OUT%geometry-diff.png" (
  echo [FAIL] geometry-diff.png missing
  set FAIL=1
) else (
  echo [OK] geometry-diff.png present
)

echo [check] negative: missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode shadow --model NoSuchModel.nmesh --frames 8 --no-hud --size 320x240 > "%OUT%verify-negative.txt" 2>&1
if errorlevel 1 (
  echo [OK] negative exited nonzero
) else (
  echo [FAIL] negative silently accepted
  set FAIL=1
)

echo FINAL_FAIL=%FAIL%
if "%FAIL%"=="1" (
  echo VERIFY FAILED
  exit /b 1
)
echo VERIFY OK
exit /b 0

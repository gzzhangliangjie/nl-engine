@echo off
setlocal
rem M14A1 (S14-A) soft-raster full-software baseline acceptance.
rem ASCII only; CRLF; no parentheses inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem soft-raster is deterministic (vis buffer u64 atomicMin is order-
rem insensitive; no RNG in the chain) -> bytes gate vs frozen baseline.
rem Assertions:
rem   - re-shot --mode soft-raster on both backends exits 0; dx12 deviceErrors=0
rem   - re-shot bytes-identical to frozen baseline (both backends)
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --size 960x540 --yaw 12 --pitch 9 --dist 7 --frames 128 --no-hud"
set FAIL=0

echo [shot] re-shoot soft-raster on both backends (dx12 log captured)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode soft-raster %ARGS% --screenshot "%OUT%verify-soft-%%B.png" > "%OUT%verify-log.txt" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode soft-raster %ARGS% --screenshot "%OUT%verify-soft-%%B.png" >nul 2>&1
  )
  if errorlevel 1 (
    echo [FAIL] soft-raster-%%B viewer exited nonzero
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
  fc /b "%OUT%soft-%%B.png" "%OUT%verify-soft-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] soft-raster-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] soft-raster-%%B bytes identical to baseline
  )
)

echo [negative] missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode soft-raster --model missing.nmesh --frames 1 --no-hud --screenshot "%OUT%verify-negative.png" >nul 2>&1
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

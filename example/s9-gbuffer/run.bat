@echo off
setlocal
rem M9.0 / S9 - GBuffer extraction + compute direct lighting acceptance shots.
rem ASCII only; CRLF; no parentheses inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem Evidence chain:
rem   - Q4 plan A: no standalone GBuffer texture. direct.slang compute 8x8
rem     calls gbInterpolateVisTriangle on demand, then the mode 5 BRDF.
rem   - --mode shadow and --mode pulling use the SAME camera and params;
rem     their PNGs must be byte-identical on each backend.
rem   - Empty pixels: PS discards to the framebuffer clear color; the compute
rem     UAV writes the same per-frame clear color via b3 DirectConstants.
rem     --color 2 must change the image, proving it is not hardcoded.
rem   - pulling shot must still match the frozen s7.2 baseline byte for byte.
rem   - Negative: a missing model must exit nonzero, never silently fall back.

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --shading 5 --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 9"
set FAIL=0

echo [shot] re-shoot shadow and pulling on both backends ...
for %%B in (dx12 vulkan) do (
  for %%M in (shadow pulling) do (
    "%ROOT%\build\%%B\viewer.exe" --mode %%M %ARGS% --screenshot "%OUT%verify-%%M-%%B.png" >nul
    if errorlevel 1 (
      echo [FAIL] %%M-%%B viewer exited nonzero
      set FAIL=1
    )
  )
)

echo [check] bytes: re-shot vs frozen baselines ...
for %%M in (shadow pulling) do for %%B in (dx12 vulkan) do (
  fc /b "%OUT%%%M-%%B.png" "%OUT%verify-%%M-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] %%M-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] %%M-%%B bytes identical to baseline
  )
)

echo [check] core acceptance: shadow vs pulling byte-identical ...
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-shadow-%%B.png" "%OUT%verify-pulling-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] shadow vs pulling differ on %%B
    set FAIL=1
  ) else (
    echo [OK] shadow vs pulling byte-identical on %%B
  )
)

echo [check] old path untouched: pulling matches s7.2 frozen baseline ...
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%verify-pulling-%%B.png" "%ROOT%\example\s7-done\materials-pulling-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] pulling differs from s7.2 baseline on %%B
    set FAIL=1
  ) else (
    echo [OK] pulling matches s7.2 baseline on %%B
  )
)

echo [check] clear color: --color 2 changes pixels and keeps parity ...
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode shadow %ARGS% --color 2 --screenshot "%OUT%verify-shadow-c2-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] shadow color2 %%B exited nonzero
    set FAIL=1
  )
  "%ROOT%\build\%%B\viewer.exe" --mode pulling %ARGS% --color 2 --screenshot "%OUT%verify-pulling-c2-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] pulling color2 %%B exited nonzero
    set FAIL=1
  )
  fc /b "%OUT%verify-shadow-c2-%%B.png" "%OUT%verify-pulling-c2-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] color2 parity broken on %%B
    set FAIL=1
  ) else (
    echo [OK] color2 parity on %%B
  )
  fc /b "%OUT%verify-shadow-c2-%%B.png" "%OUT%verify-shadow-%%B.png" >nul
  if errorlevel 1 (
    echo [OK] color2 changed the image on %%B
  ) else (
    echo [FAIL] color2 had no effect on %%B - clear color hardcoded
    set FAIL=1
  )
)

echo [check] log anchor: shadow scene ready ...
"%ROOT%\build\dx12\viewer.exe" --mode shadow --model Suzanne-materials.nmesh --shading 5 --screenshot "%OUT%verify-log-dx12.png" --frames 8 --no-hud --size 320x240 > "%OUT%verify-log.txt" 2>&1
findstr /C:"shadow scene ready" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] shadow scene ready log line missing
  set FAIL=1
) else (
  echo [OK] shadow scene ready log line present
)

echo [check] negative: missing model must exit nonzero ...
"%ROOT%\build\dx12\viewer.exe" --mode shadow --model NoSuchModel.nmesh --frames 8 --no-hud --size 320x240 > "%OUT%verify-negative.txt" 2>&1
if errorlevel 1 (
  echo [OK] negative exited nonzero
) else (
  echo [FAIL] negative silently accepted
  set FAIL=1
)
findstr /C:"cannot open for read" "%OUT%verify-negative.txt" >nul
if errorlevel 1 (
  echo [FAIL] negative error log missing
  set FAIL=1
) else (
  echo [OK] negative error log present
)

if "%FAIL%"=="1" (
  echo VERIFY FAILED
  exit /b 1
)
echo VERIFY OK
exit /b 0

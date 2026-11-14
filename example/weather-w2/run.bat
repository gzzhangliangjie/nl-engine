@echo off
setlocal
rem WEA M-WEA2 - cloud transmittance + analytic height fog (5 consumers).
rem ASCII only; CRLF; no parentheses inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem What we assert:
rem   - 5 cases re-shot on both backends, exit 0, deviceErrors=0
rem   - re-shots byte-identical to frozen baselines (deterministic)
rem   - negative: unknown preset exits 2

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

echo [shot] overcast-noon both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode probe --model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --sky-bg --weather-seed 7 --weather-preset overcast --weather-t 300 --screenshot "%OUT%verify-overcast-noon-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] overcast-noon-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] overcast-noon bytes vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%overcast-noon-%%B.png" "%OUT%verify-overcast-noon-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] overcast-noon-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] overcast-noon-%%B bytes identical to baseline
  )
)

echo [shot] fog-showcase both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode probe --model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --sky-bg --weather-seed 7 --weather-preset clear --weather-t 300 --weather-fog-density 0.05 --weather-fog-height 8 --screenshot "%OUT%verify-fog-showcase-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] fog-showcase-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] fog-showcase bytes vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%fog-showcase-%%B.png" "%OUT%verify-fog-showcase-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] fog-showcase-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] fog-showcase-%%B bytes identical to baseline
  )
)

echo [shot] pviz-overcast both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode probe-viz --model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --weather-seed 7 --weather-preset overcast --weather-t 300 --screenshot "%OUT%verify-pviz-overcast-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] pviz-overcast-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] pviz-overcast bytes vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%pviz-overcast-%%B.png" "%OUT%verify-pviz-overcast-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] pviz-overcast-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] pviz-overcast-%%B bytes identical to baseline
  )
)

echo [shot] refgi-overcast both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode reference-gi --model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --weather-seed 7 --weather-preset overcast --weather-t 300 --screenshot "%OUT%verify-refgi-overcast-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] refgi-overcast-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] refgi-overcast bytes vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%refgi-overcast-%%B.png" "%OUT%verify-refgi-overcast-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] refgi-overcast-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] refgi-overcast-%%B bytes identical to baseline
  )
)

echo [shot] cacheviz-overcast both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode cache-viz --model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --weather-seed 7 --weather-preset overcast --weather-t 300 --screenshot "%OUT%verify-cacheviz-overcast-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] cacheviz-overcast-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] cacheviz-overcast bytes vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%cacheviz-overcast-%%B.png" "%OUT%verify-cacheviz-overcast-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] cacheviz-overcast-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] cacheviz-overcast-%%B bytes identical to baseline
  )
)

echo [negative] unknown preset must exit 2
"%ROOT%\build\dx12\viewer.exe" --mode probe --model Suzanne-materials.nmesh --frames 2 --no-hud --size 320x240 --weather-seed 7 --weather-preset bogus > "%OUT%verify-negative.txt" 2>&1
findstr /C:"unknown --weather-preset" "%OUT%verify-negative.txt" >nul
if errorlevel 1 (
  echo [FAIL] negative preset anchor missing
  set FAIL=1
) else (
  echo [OK] negative preset anchor present
)

if "%FAIL%"=="1" (
  echo VERIFY FAILED
  exit /b 1
)
echo VERIFY ALL GREEN
exit /b 0

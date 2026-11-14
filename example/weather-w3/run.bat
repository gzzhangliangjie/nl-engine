@echo off
setlocal
rem WEA M-WEA3 - wet materials (engine-level, N.up weighted) + history reject.
rem ASCII only; CRLF; no parentheses inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

echo [shot] wet-early both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode probe --sky-bg --model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --weather-seed 7 --weather-preset rain --weather-t 300 --weather-wetness 0.1 --screenshot "%OUT%verify-wet-early-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] wet-early-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] wet-early bytes vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%wet-early-%%B.png" "%OUT%verify-wet-early-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] wet-early-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] wet-early-%%B bytes identical to baseline
  )
)

echo [shot] wet-soaked both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode probe --model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --sky-bg --weather-seed 7 --weather-preset rain --weather-t 300 --screenshot "%OUT%verify-wet-soaked-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] wet-soaked-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] wet-soaked bytes vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%wet-soaked-%%B.png" "%OUT%verify-wet-soaked-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] wet-soaked-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] wet-soaked-%%B bytes identical to baseline
  )
)

echo [shot] pviz-wet both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode probe-viz --model Suzanne-materials.nmesh --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 7 --weather-seed 7 --weather-preset rain --weather-t 300 --screenshot "%OUT%verify-pviz-wet-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] pviz-wet-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] pviz-wet bytes vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%pviz-wet-%%B.png" "%OUT%verify-pviz-wet-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] pviz-wet-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] pviz-wet-%%B bytes identical to baseline
  )
)

echo [tool] reject-filter both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode filter --filter-gi-dump --model Suzanne-materials.nmesh --frames 16 --no-hud --size 480x270 --weather-seed 7 --weather-preset rain --weather-t 0 --weather-t-step 30.0 > "%OUT%verify-reject-filter-%%B.txt" 2>&1
  if errorlevel 1 (
    echo [FAIL] reject-filter-%%B exited nonzero
    set FAIL=1
  )
  findstr /C:"history reject" "%OUT%verify-reject-filter-%%B.txt" >nul
  if errorlevel 1 (
    echo [FAIL] reject-filter-%%B anchor missing: history reject
    set FAIL=1
  ) else (
    echo [OK] reject-filter-%%B anchor present
  )
)

echo [tool] steady-filter both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode filter --filter-gi-dump --model Suzanne-materials.nmesh --frames 32 --no-hud --size 480x270 --weather-seed 7 --weather-preset rain --weather-t 300 > "%OUT%verify-steady-filter-%%B.txt" 2>&1
  if errorlevel 1 (
    echo [FAIL] steady-filter-%%B exited nonzero
    set FAIL=1
  )
)

if "%FAIL%"=="1" (
  echo VERIFY FAILED
  exit /b 1
)
echo VERIFY ALL GREEN
exit /b 0

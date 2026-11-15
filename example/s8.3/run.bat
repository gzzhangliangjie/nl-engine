@echo off
setlocal EnableExtensions
rem M-RT0 (S8.3) rear-view mirror render-to-texture acceptance.
rem --mirror: Scene draws sceneBatches_ a second time with a rear-facing camera
rem (yaw+180) into a half-res RT, then blits it to the bottom-right corner.
rem Flag is OFF by default: all legacy screenshots stay byte-identical.
rem
rem Usage: run.bat verify
rem Evidence: shot mirror-scene on both backends + bytes match baselines +
rem           "mirror pass ready" log anchor + flag-off byte identity check.
set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "SCENE=%ROOT%\example\night-demo\night-demo.scene"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] mirror-scene on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode scene --scene "%SCENE%" --mirror --no-hud --screenshot "%OUT%verify-mirror-scene-%%B.png" --frames 8 --size 960x540 --yaw 40 --pitch 11 --dist 12 > "%OUT%verify-mirror-%%B.log" 2>&1
  if errorlevel 1 (
    echo [FAIL] mirror-scene %%B viewer exited nonzero
    set FAIL=1
  )
  findstr /C:"mirror pass ready" "%OUT%verify-mirror-%%B.log" >nul
  if errorlevel 1 (
    echo [FAIL] mirror pass ready anchor missing on %%B
    set FAIL=1
  ) else (
    echo [OK] mirror pass ready anchor on %%B
  )
)

echo [check] bytes: verify shots vs baselines ...
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%mirror-scene-%%B.png" "%OUT%verify-mirror-scene-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] mirror-scene-%%B bytes differ
    set FAIL=1
  ) else (
    echo [OK] mirror-scene-%%B bytes identical
  )
)

if "%FAIL%"=="1" (
  echo ALL RED [s8.3]
  exit /b 1
)
echo ALL GREEN [s8.3]
endlocal

@echo off
setlocal
rem M5.1 (S5.1) DAG traversal acceptance shots. ASCII only; !VAR! delayed expansion
rem inside for blocks (M4.1 lesson: %VAR% parses empty and silently no-ops).
rem Usage: run.bat verify   (re-shoots near/far pairs for both backends, exit 0 = ok)

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] near %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode dag --hud --screenshot "%OUT%s5-traverse-near-%%B.png" --frames 8 --shading normal --yaw 12 --pitch 9 --dist 4.2 --cut-thr 1.0 --size 640x720
  if errorlevel 1 set FAIL=1
  echo [shot] far %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode dag --screenshot "%OUT%s5-traverse-far-%%B.png" --frames 8 --shading normal --yaw 12 --pitch 9 --dist 20 --cut-thr 16 --size 640x720
  if errorlevel 1 set FAIL=1
)

rem Machine-readable evidence: near must emit all 237 leaf clusters; far (dist 20,
rem threshold 16 px) must collapse to 14. The counts land in the log lines.
echo [check] near count (expect 237) ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --screenshot "%OUT%verify-near-dx12.png" --frames 8 --shading normal --yaw 12 --pitch 9 --dist 4.2 --cut-thr 1.0 --size 640x720 | findstr /C:"visible 237 / 324"
if errorlevel 1 set FAIL=1
echo [check] far count (expect 14) ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --screenshot "%OUT%verify-far-dx12.png" --frames 8 --shading normal --yaw 12 --pitch 9 --dist 20 --cut-thr 16 --size 640x720 | findstr /C:"visible 14 / 324"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s5-traverse verify
  exit /b 1
)
echo [OK] s5-traverse verify: near pair + far pair + count checks
exit /b 0

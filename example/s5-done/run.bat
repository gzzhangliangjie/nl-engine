@echo off
setlocal
rem M5.2 (S5.2) GPU triangle count acceptance shots. ASCII only; !VAR! delayed
rem expansion inside for blocks (M4.1 lesson: %VAR% parses empty and silently no-ops).
rem Usage: run.bat verify   (re-shoots near/far HUD pairs for both backends, exit 0 = ok)

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] near %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode dag --hud --screenshot "%OUT%s5-tris-near-%%B.png" --frames 8 --shading normal --yaw 12 --pitch 9 --dist 4.2 --cut-thr 1.0 --size 640x720
  if errorlevel 1 set FAIL=1
  echo [shot] far %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode dag --hud --screenshot "%OUT%s5-tris-far-%%B.png" --frames 8 --shading normal --yaw 12 --pitch 9 --dist 20 --cut-thr 16 --size 640x720
  if errorlevel 1 set FAIL=1
)

rem Machine-readable evidence: the GPU atomic counter lands in the log line.
rem near must count every finest triangle (3936 == triangles(finest)); far
rem (dist 20, threshold 16 px) must collapse to 426.
echo [check] near tris (expect 3936) ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --screenshot "%OUT%verify-near-dx12.png" --frames 8 --shading normal --yaw 12 --pitch 9 --dist 4.2 --cut-thr 1.0 --size 640x720 | findstr /C:"tris 3936"
if errorlevel 1 set FAIL=1
echo [check] far tris (expect 426) ...
"%ROOT%\build\dx12\viewer.exe" --mode dag --screenshot "%OUT%verify-far-dx12.png" --frames 8 --shading normal --yaw 12 --pitch 9 --dist 20 --cut-thr 16 --size 640x720 | findstr /C:"tris 426"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s5-done verify
  exit /b 1
)
echo [OK] s5-done verify: near pair + far pair + tris checks
exit /b 0

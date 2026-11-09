@echo off
setlocal
rem M6.0 (S6.0) Hi-Z depth prepass + pyramid acceptance shots.
rem M6.1 correction (2026-11-09, user-approved): occlusion culling compares the
rem cluster's nearest point against the region's FARTHEST surface (UE
rem IsVisibleHZB / bevy occlusion_cull_screen_aabb authority) -> the pyramid is
rem now MAX of radial distance (was min). Top readout semantics changed:
rem background pixels (clear value 1e9) legitimately dominate -> "top far
rem background-dominates" instead of a real value unless the model fills the
rem frame. ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).
rem Usage: run.bat verify   (re-shoots near/far viz pairs for both backends, exit 0 = ok)

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] near %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --hud --screenshot "%OUT%hiz-viz-near-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 set FAIL=1
  echo [shot] far %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode hiz --hud --screenshot "%OUT%hiz-viz-far-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 20 --size 640x720
  if errorlevel 1 set FAIL=1
)

rem Machine-readable evidence (readback ring, delayed 2 frames):
rem  - near: hiz self-occlusion culls Suzanne's back clusters -> 233 / 324
rem    (dag baseline without hiz emits 237 / 324; the 4-cluster delta is the
rem    convex-shape self-occlusion margin, both measured on this machine)
rem  - far: cut collapses coarse, no self-occlusion -> 237 / 324 == dag
rem  - top far readback chain live: background-dominates (max-pyramid semantics)
echo [check] near hiz self-occlusion (expect 233 / 324) ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --hud --screenshot "%OUT%verify-near-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720 | findstr /C:"visible 233 / 324"
if errorlevel 1 set FAIL=1
echo [check] far hiz coarse cut (expect 237 / 324) ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --hud --screenshot "%OUT%verify-far-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 20 --size 640x720 | findstr /C:"visible 237 / 324"
if errorlevel 1 set FAIL=1
echo [check] top far readback chain (expect background-dominates) ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --hud --screenshot "%OUT%verify-top-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720 | findstr /C:"background-dominates"
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] s6-hiz verify
  exit /b 1
)
echo [OK] s6-hiz verify: near pair + far pair + hiz 233/237 per-324 + top far chain checks
exit /b 0

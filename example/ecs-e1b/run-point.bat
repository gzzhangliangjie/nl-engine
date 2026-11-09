@echo off
setlocal
rem M-E1b.2 (P8 ECS E1b): point lights -- 7 objects x 3 colored point lights + 1 fill directional.
rem LightComponent gains type/position/range; t6 light buffer becomes 3x float4 per light.
rem UE DeferredLightingCommon.ush:480-486 inverse-squared attenuation + radius mask.
rem Usage: run-point.bat   (re-shoots both backends + machine assertions, exit 0 = ok)
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (DX12 VULKAN) do (
  echo [shot] ecs-e1b-point %%B ...
  "%ROOT%\build\%%B\viewer.exe" --scene "%OUT%ecs-e1b-point.scene" --screenshot "%OUT%shot-point-%%B.png" --frames 8 --size 960x540 --no-hud --yaw 12 --pitch 9 --dist 18 > "%OUT%shot-point-%%B.log" 2>&1
  if errorlevel 1 set FAIL=1
  echo [check] %%B log: 7 objects / 4 lights / 3 point / zero device errors ...
  findstr /C:"ecs scene: 7 objects" /C:"4 lights" /C:"3 point" "%OUT%shot-point-%%B.log" >nul
  if errorlevel 1 set FAIL=1
  findstr /C:"deviceErrors=0" "%OUT%shot-point-%%B.log" >nul
  if errorlevel 1 set FAIL=1
  if not exist "%OUT%shot-point-%%B.png" set FAIL=1
)

if %FAIL%==1 (
  echo [FAIL] ecs-e1b-point verify
  exit /b 1
)
echo [OK] ecs-e1b-point verify: 7 objects / 4 lights / 3 point / zero device errors on both backends
exit /b 0

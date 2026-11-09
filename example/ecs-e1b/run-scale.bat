@echo off
setlocal
rem M-E1b.1 (P8 ECS E1b): ECS scale anchor -- 50 objects x 8 directional lights.
rem 10x5 grid over 4 mesh files => 4 instanced batches (13/13/12/12), 58 entities
rem in the World JSON. Each batch owns its t5 instance buffer (M-E1b.1 fix:
rem DX12 SV_InstanceID does NOT include StartInstanceLocation).
rem Usage: run-scale.bat   (re-shoots both backends + machine assertions, exit 0 = ok)
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (DX12 VULKAN) do (
  echo [shot] ecs-e1b-scale %%B ...
  "%ROOT%\build\%%B\viewer.exe" --scene "%OUT%ecs-e1b-scale.scene" --screenshot "%OUT%shot-scale-%%B.png" --frames 8 --size 960x540 --no-hud --yaw 12 --pitch 9 --dist 18 > "%OUT%shot-scale-%%B.log" 2>&1
  if errorlevel 1 set FAIL=1
  echo [check] %%B log: 50 objects / 8 lights / 4 batches / zero device errors ...
  findstr /C:"ecs scene: 50 objects" /C:"8 lights" "%OUT%shot-scale-%%B.log" >nul
  if errorlevel 1 set FAIL=1
  findstr /C:"batches=4" "%OUT%shot-scale-%%B.log" >nul
  if errorlevel 1 set FAIL=1
  findstr /C:"deviceErrors=0" "%OUT%shot-scale-%%B.log" >nul
  if errorlevel 1 set FAIL=1
  if not exist "%OUT%shot-scale-%%B.png" set FAIL=1
)

if %FAIL%==1 (
  echo [FAIL] ecs-e1b-scale verify
  exit /b 1
)
echo [OK] ecs-e1b-scale verify: 50 objects / 8 lights / 4 batches / zero device errors on both backends
exit /b 0

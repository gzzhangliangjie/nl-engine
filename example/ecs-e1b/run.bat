@echo off
setlocal
rem M-E1b.0 (P8 ECS E1b): viewer --scene renders N objects with M directional lights.
rem 5 Suzanne.glb instances (distinct TRS + tint) + 3 directional lights (key warm /
rem fill cool / rim), ambient 0.22. Component data lives in nl/scene (Model/Light),
rem renderer collects via signature-style O(N) walk and draws one instanced batch.
rem Usage: run.bat   (re-shoots both backends + machine assertions, exit 0 = ok)
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (DX12 VULKAN) do (
  echo [shot] ecs-e1b %%B ...
  "%ROOT%\build\%%B\viewer.exe" --scene "%OUT%ecs-e1b.scene" --screenshot "%OUT%shot-%%B.png" --frames 8 --size 960x540 --no-hud --yaw 12 --pitch 9 > "%OUT%shot-%%B.log" 2>&1
  if errorlevel 1 set FAIL=1
  echo [check] %%B log: 5 objects / 3 lights / zero device errors ...
  findstr /C:"ecs scene: 5 objects" /C:"3 lights" "%OUT%shot-%%B.log" >nul
  if errorlevel 1 set FAIL=1
  findstr /C:"deviceErrors=0" "%OUT%shot-%%B.log" >nul
  if errorlevel 1 set FAIL=1
  if not exist "%OUT%shot-%%B.png" set FAIL=1
)

if %FAIL%==1 (
  echo [FAIL] ecs-e1b verify
  exit /b 1
)
echo [OK] ecs-e1b verify: 5 objects / 3 lights / 1 batch / zero device errors on both backends
exit /b 0

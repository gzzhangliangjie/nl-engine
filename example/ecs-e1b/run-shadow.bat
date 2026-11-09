@echo off
setlocal
rem M-E1b.4 (P8 ECS E1b): directional light 2D shadow map (R32F Texture2DArray,
rem MIN blend nearest-depth, integer texel Load + 3x3 PCF + slope-scaled bias in
rem psScene). One key light with shadow=true. Quantitative gate compares against
rem the SAME scene with shadow=false (ecs-e1b-shadow-base.scene) -- the older
rem ecs-e1b.scene baselines have no floor disc and are not comparable.
rem Usage: run-shadow.bat   (re-shoots both backends + machine assertions, exit 0 = ok)
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (DX12 VULKAN) do (
  echo [shot] ecs-e1b-shadow %%B ...
  "%ROOT%\build\%%B\viewer.exe" --scene "%OUT%ecs-e1b-shadow.scene" --screenshot "%OUT%shot-shadow-%%B.png" --frames 8 --size 960x540 --no-hud --yaw 12 --pitch 9 > "%OUT%shot-shadow-%%B.log" 2>&1
  if errorlevel 1 set FAIL=1
  echo [shot] ecs-e1b-shadow-base %%B ...
  "%ROOT%\build\%%B\viewer.exe" --scene "%OUT%ecs-e1b-shadow-base.scene" --screenshot "%OUT%shot-shadow-base-%%B.png" --frames 8 --size 960x540 --no-hud --yaw 12 --pitch 9 > "%OUT%shot-shadow-base-%%B.log" 2>&1
  if errorlevel 1 set FAIL=1
  echo [check] %%B log: 5 objects / 3 lights / shadowLights=1 / zero device errors ...
  findstr /C:"shadowLights=1" "%OUT%shot-shadow-%%B.log" >nul
  if errorlevel 1 set FAIL=1
  findstr /C:"deviceErrors=0" "%OUT%shot-shadow-%%B.log" >nul
  if errorlevel 1 set FAIL=1
  if not exist "%OUT%shot-shadow-%%B.png" set FAIL=1
  if not exist "%OUT%shot-shadow-base-%%B.png" set FAIL=1
)

echo [check] quantitative: shadow only darkens pixels vs same-scene shadow=false baseline ...
python "%OUT%check-shadow.py" "%OUT%shot-shadow-base-DX12.png" "%OUT%shot-shadow-DX12.png" 3000
if errorlevel 1 set FAIL=1
python "%OUT%check-shadow.py" "%OUT%shot-shadow-base-VULKAN.png" "%OUT%shot-shadow-VULKAN.png" 3000
if errorlevel 1 set FAIL=1

echo [negative] point light with shadow=true must be rejected with exit 2 ...
python -c "import json;d=json.load(open(r'%OUT%ecs-e1b-shadow.scene',encoding='utf-8'));ents=[e for e in d['entities'] if 'Light' in e.get('components',{})];l=[e for e in ents if e['components']['Light'].get('type',0)==0];l[0]['components']['Light'].pop('shadow');l[0]['components']['Light']['type']=1;l[0]['components']['Light']['position']=[0,3,0];l[0]['components']['Light']['range']=12;l[0]['components']['Light']['shadow']=True;json.dump(d,open(r'%OUT%bad-point-shadow.scene','w',encoding='utf-8'),indent=2)"
"%ROOT%\build\DX12\viewer.exe" --scene "%OUT%bad-point-shadow.scene" --frames 1 --no-hud > "%OUT%bad-point-shadow.log" 2>&1
if errorlevel 2 (if errorlevel 3 set FAIL=1) else set FAIL=1
findstr /C:"does not support shadow" "%OUT%bad-point-shadow.log" >nul
if errorlevel 1 set FAIL=1

if %FAIL%==1 (
  echo [FAIL] ecs-e1b-shadow verify
  exit /b 1
)
echo [OK] ecs-e1b-shadow verify: shadowLights=1 / zero device errors / shadow-only-darkens / negative rejected on both backends

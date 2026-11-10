@echo off
rem M-GL4：宿主级脚本 OnTick 接逻辑帧 —— 验收（双后端）。
rem 用法：仓库根执行 example\gl4\run.bat；依赖 viewer.exe 双后端 + dotnet SDK。
rem 退出码：0 = 全绿；非 0 = 断言失败（findstr / verify.py）。
rem 编码：UTF-8 无 BOM + CRLF，中文注释直接写；禁用 chcp（换页后 cmd 解析
rem 偏移错位，4 种模式实测复现；rem 行 mojibake 只是显示难看不影响执行）。
setlocal
set "ROOT=E:/UE_STUDY_2026/nl-engine-gameloop"
set "DX=%ROOT%/build/DX12/viewer.exe"
set "VK=%ROOT%/build/VULKAN/viewer.exe"
set "SCENE=%ROOT%/example/gl4/gl4-orbit.scene"
set "PROJ=%ROOT%/script/examples/orbit"
set "OUT=%ROOT%/example/gl4"
set "VERIFY=%ROOT%/example/gl4/verify.py"

if not exist "%DX%" ( echo [ERROR] DX12 viewer.exe missing & exit /b 1 )
if not exist "%VK%" ( echo [ERROR] VULKAN viewer.exe missing & exit /b 1 )

echo === M-GL4 build script example ===
dotnet build "%ROOT%/script/bootstrap" > "%OUT%\gl4-bootstrap-build.log" 2>&1
dotnet build "%PROJ%" > "%OUT%\gl4-dotnet-build.log" 2>&1
if errorlevel 1 ( echo [ERROR] dotnet build failed & exit /b 1 )

echo === M-GL4 tick suite: fixed-dt 16.7 x 100 frames, script orbit r=1.6 w=0.9 ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --script "%PROJ%" --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --screenshot "%OUT%\gl4-tick-DX12.png" > "%OUT%\gl4-tick-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] tick DX12 FAILED & exit /b 1 )
findstr /c:"script: loaded" "%OUT%\gl4-tick-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] tick DX12: script loaded line missing & exit /b 1 )
findstr /c:"script probe: ticks=100 " "%OUT%\gl4-tick-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] tick DX12: probe ticks=100 missing & exit /b 1 )

"%VK%" --mode scene --scene "%SCENE%" --tick parallel --script "%PROJ%" --fixed-dt 16.7 --frames 100 --size 960x540 --no-hud --yaw 12 --pitch 9 --screenshot "%OUT%\gl4-tick-VULKAN.png" > "%OUT%\gl4-tick-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] tick VULKAN FAILED & exit /b 1 )
findstr /c:"script probe: ticks=100 " "%OUT%\gl4-tick-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] tick VULKAN: probe ticks=100 missing & exit /b 1 )

echo === M-GL4 mid suite: fixed-dt 30 x 3 frames - per-frame ticks [1,2,2], total 5 ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --script "%PROJ%" --fixed-dt 30 --frames 3 --size 960x540 --no-hud --yaw 12 --pitch 9 --screenshot "%OUT%\gl4-mid-DX12.png" > "%OUT%\gl4-mid-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] mid DX12 FAILED & exit /b 1 )
findstr /c:"script probe: ticks=5 " "%OUT%\gl4-mid-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] mid DX12: probe ticks=4 missing & exit /b 1 )

"%VK%" --mode scene --scene "%SCENE%" --tick parallel --script "%PROJ%" --fixed-dt 30 --frames 3 --size 960x540 --no-hud --yaw 12 --pitch 9 --screenshot "%OUT%\gl4-mid-VULKAN.png" > "%OUT%\gl4-mid-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] mid VULKAN FAILED & exit /b 1 )
findstr /c:"script probe: ticks=5 " "%OUT%\gl4-mid-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] mid VULKAN: probe ticks=4 missing & exit /b 1 )

echo === M-GL4 pause suite: game.pause before loop - ticks=0, position frozen at scene initial ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --script "%PROJ%" --fixed-dt 16.7 --frames 5 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "game.pause" --screenshot "%OUT%\gl4-pause-DX12.png" > "%OUT%\gl4-pause-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] pause DX12 FAILED & exit /b 1 )
findstr /c:"script probe: ticks=0 " "%OUT%\gl4-pause-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] pause DX12: probe ticks=0 missing & exit /b 1 )

"%VK%" --mode scene --scene "%SCENE%" --tick parallel --script "%PROJ%" --fixed-dt 16.7 --frames 5 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "game.pause" --screenshot "%OUT%\gl4-pause-VULKAN.png" > "%OUT%\gl4-pause-VULKAN.log" 2>&1
if errorlevel 1 ( echo [ERROR] pause VULKAN FAILED & exit /b 1 )
findstr /c:"script probe: ticks=0 " "%OUT%\gl4-pause-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] pause VULKAN: probe ticks=0 missing & exit /b 1 )

echo === M-GL4 smoke: game.script status with script loaded ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --script "%PROJ%" --fixed-dt 16.7 --frames 2 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "game.script" --screenshot "%OUT%\gl4-smoke-DX12.png" > "%OUT%\gl4-smoke-DX12.log" 2>&1
if errorlevel 1 ( echo [ERROR] smoke DX12 FAILED & exit /b 1 )
findstr /c:"tick=on count=0" "%OUT%\gl4-smoke-DX12.log" >nul
if errorlevel 1 ( echo [ERROR] smoke: game.script status line missing & exit /b 1 )

echo === M-GL4 negative: game.script without --script - must fail (exit 1) ===
"%DX%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 2 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "game.script" --screenshot "%OUT%\gl4-neg.png" > "%OUT%\gl4-neg.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative: viewer should exit 1 but exited 0 & exit /b 1 )
findstr /c:"no script loaded" "%OUT%\gl4-neg.log" >nul
if errorlevel 1 ( echo [ERROR] negative: expected failure message missing & exit /b 1 )

"%VK%" --mode scene --scene "%SCENE%" --tick parallel --fixed-dt 16.7 --frames 2 --size 960x540 --no-hud --yaw 12 --pitch 9 --console-exec "game.script" --screenshot "%OUT%\gl4-neg-VULKAN.png" > "%OUT%\gl4-neg-VULKAN.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative VK: viewer should exit 1 but exited 0 & exit /b 1 )
findstr /c:"no script loaded" "%OUT%\gl4-neg-VULKAN.log" >nul
if errorlevel 1 ( echo [ERROR] negative VK: expected failure message missing & exit /b 1 )

echo === M-GL4 verify twin ===
python "%VERIFY%" "%OUT%\gl4-tick-DX12.log" "%OUT%\gl4-tick-VULKAN.log" "%OUT%\gl4-tick-DX12.png" "%OUT%\gl4-tick-VULKAN.png" "%OUT%\gl4-mid-DX12.log" "%OUT%\gl4-mid-VULKAN.log" "%OUT%\gl4-pause-DX12.log" "%OUT%\gl4-pause-VULKAN.log" "%OUT%\gl4-neg.log"
if errorlevel 1 ( echo [ERROR] verify twin FAILED & exit /b 1 )

echo M-GL4 ALL GREEN
endlocal
exit /b 0

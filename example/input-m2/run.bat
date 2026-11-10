@echo off
rem P8 M-I2 acceptance: input-hud debug panel (injection-path driven).
rem Five-stage tool-card convention (M-I0/M-I1 same shape):
rem   1. dual-backend selftest of the underlying tool gates (viewer 16/16)
rem   2. regenerate both backend baseline PNGs from the real demo script
rem   3. byte-compare regenerated PNGs against the committed baselines
rem Evidence rule: the panel content is driven by --input-demo (frame-indexed
rem synthetic injections), so the PNGs are deterministic and byte-reproducible.

setlocal
set "HERE=%~dp0"
set "ROOT=%~dp0..\.."

echo [1/3] viewer selftest DX12 + VULKAN ...
"%ROOT%\build\DX12\viewer.exe" --selftest > "%HERE%selftest-dx12.log" 2>&1
if errorlevel 1 goto :fail
findstr /c:"SELFTEST PASS (16/16)" "%HERE%selftest-dx12.log" >nul
if errorlevel 1 goto :fail
"%ROOT%\build\VULKAN\viewer.exe" --selftest > "%HERE%selftest-vulkan.log" 2>&1
if errorlevel 1 goto :fail
findstr /c:"SELFTEST PASS (16/16)" "%HERE%selftest-vulkan.log" >nul
if errorlevel 1 goto :fail

echo [2/3] regenerate input-hud demo screenshots (dual backend) ...
"%ROOT%\build\DX12\viewer.exe" --mode input-hud --input-demo --frames 8 --size 960x540 --screenshot "%HERE%acceptance-dx12.png" > "%HERE%screenshot-dx12.log" 2>&1
if errorlevel 1 goto :fail
findstr /c:"input-demo: inject key 256 at iter 8" "%HERE%screenshot-dx12.log" >nul
if errorlevel 1 goto :fail
findstr /c:"screenshot written" "%HERE%screenshot-dx12.log" >nul
if errorlevel 1 goto :fail
"%ROOT%\build\VULKAN\viewer.exe" --mode input-hud --input-demo --frames 8 --size 960x540 --screenshot "%HERE%acceptance-vulkan.png" > "%HERE%screenshot-vulkan.log" 2>&1
if errorlevel 1 goto :fail
findstr /c:"input-demo: inject key 256 at iter 8" "%HERE%screenshot-vulkan.log" >nul
if errorlevel 1 goto :fail

echo [3/3] byte-compare regenerated PNGs vs committed baselines ...
fc /b "%HERE%acceptance-dx12.png" "%HERE%acceptance-dx12.expected.png" >nul
if errorlevel 1 goto :fail
fc /b "%HERE%acceptance-vulkan.png" "%HERE%acceptance-vulkan.expected.png" >nul
if errorlevel 1 goto :fail

echo [M-I2] input-hud acceptance: ALL GREEN
echo   - viewer selftest 16/16 x2
echo   - demo script anchors hit (Esc injected at capture frame, dual backend)
echo   - PNG baselines byte-identical (fc /b)
exit /b 0

:fail
echo [ERROR] input-m2 acceptance FAILED - see logs in example\input-m2
exit /b 1

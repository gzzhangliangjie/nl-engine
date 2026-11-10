@echo off
REM M3.1 / s3-fallback 复现：链再生成（lod0 带 v3 fallback 尾段）-> 离线渲染证据图
REM 注意：-o 指向 build 目录，入仓资产不被触碰；fallback 段落在 lod0 尾部。
cd /d "%~dp0..\.."
build\dx12\nanite-tools.exe lod assets\meshlets\Suzanne.nmesh -o build\showcase\runs\s3-fallback\lod --max-levels 6 || exit /b 1
build\dx12\nanite-tools.exe selftest fallback || exit /b 1
python tools\fallback_preview.py build\showcase\runs\s3-fallback\lod.lod0.nmesh example\s3-fallback\fallback-preview.png || exit /b 1
echo done: example\s3-fallback\fallback-preview.png

@echo off
REM M2.2 / s2-loop 复现：标准逐层 LOD 落盘 -> 画 6 层金字塔对比图
cd /d "%~dp0..\.."
if not exist build\qem mkdir build\qem
build\dx12\nanite-tools.exe lod assets\meshlets\Suzanne.nmesh -o build\qem\lod-suzanne --max-levels 8
python tools\lod_preview.py build\qem\lod-suzanne example\s2-loop\lod-suzanne.png
echo done: example\s2-loop\lod-suzanne.png

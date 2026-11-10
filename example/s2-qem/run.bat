@echo off
REM M2.1 / s2-qem 复现：导出真实几何 -> 画简化前后对比图
cd /d "%~dp0..\.."
build\dx12\nanite-tools.exe simplify assets\meshlets\Suzanne.nmesh --obj build\qem\suzanne
python tools\qem_preview.py build\qem\suzanne example\s2-qem\qem-suzanne.png
echo done: example\s2-qem\qem-suzanne.png

@echo off
REM M3.0 / s3-dag 复现：全链校验 + JSON 导出 -> 画分层 DAG 结构图
cd /d "%~dp0..\.."
build\dx12\nanite-tools.exe dag assets\meshlets\Suzanne --export build\showcase\runs\s3-dag-export.json || exit /b 1
python tools\dag_preview.py build\showcase\runs\s3-dag-export.json example\s3-dag\dag-structure.png || exit /b 1
echo done: example\s3-dag\dag-structure.png

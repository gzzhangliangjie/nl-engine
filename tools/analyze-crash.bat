@echo off
rem 自动分析崩溃 dump（无需打开 VS）。用法见 tools/analyze-crash.py 头部。
python "%~dp0analyze-crash.py" %*

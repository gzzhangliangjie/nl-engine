@echo off
rem M-PERF0 wrapper: run check-shader-uniformity.py with python from PATH.
rem args pass through; exit code = python exit code (0 PASS / 1 violation / 2 usage).
python "%~dp0check-shader-uniformity.py" %*

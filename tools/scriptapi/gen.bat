@echo off
rem M-SC4 wrapper: run scriptapi gen.py with python from PATH.
rem args pass through; exit code = python exit code (0 ok / 1 mismatch / 2 usage).
python "%~dp0gen.py" %*

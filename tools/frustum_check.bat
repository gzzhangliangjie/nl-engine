@echo off
rem ==== frustum_check.py wrapper ==========================================
rem regression.py / server.py launch tool cases as [exe, *argv] with no
rem shell, so the Python tool needs an executable shim. cwd-independent:
rem %~dp0 resolves to this file's directory regardless of where the caller
rem runs from. Exit code passes straight through (0 ok / 1 fail).
python "%~dp0frustum_check.py" %*
exit /b %errorlevel%

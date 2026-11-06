@echo off
rem One-click launcher for the nl-engine S0 showcase.
rem Usage: double-click, or run: tools\showcase\start.bat
cd /d "%~dp0..\.."
where python >nul 2>nul
if errorlevel 1 (
    echo [ERROR] python not found in PATH. Python 3.9+ is required (standard library only).
    pause
    exit /b 1
)
echo Starting nl-engine showcase at http://127.0.0.1:8787/
echo The browser will open automatically. Press Ctrl+C here to stop the server.
python tools\showcase\server.py --port 8787 --open
pause

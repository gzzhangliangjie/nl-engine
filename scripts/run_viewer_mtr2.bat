@echo off
REM M-TR2 verification runner: set MSVC runtime env (so viewer.exe can load vcruntime/msvcp), then run viewer with args.
REM Usage: run_viewer_mtr2.bat <viewer.exe> [args...]
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\14.38.33130\x64\Microsoft.VC143.CRT;%PATH%
"%1" %2 %3 %4 %5 %6 %7 %8 %9
echo RUN_VIEWER_EXIT=%ERRORLEVEL%

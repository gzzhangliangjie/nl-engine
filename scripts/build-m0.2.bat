@echo off
setlocal
rem S0 one-shot configure + build + selftest for one backend (stage-neutral; filename kept for history).
rem Usage: scripts\build-m0.2.bat DX12   (or VULKAN)
rem NMake generator is mandatory: the VS generator breaks when HTTPS_PROXY and
rem https_proxy both exist (case-insensitive Hashtable collision in MSBuild).
rem
rem vcvars64.bat is NOT used: on this machine it calls reg.exe to locate the
rem Windows SDK, and reg.exe is blocked by the sandbox policy. All three env
rem vars are set explicitly from the paths verified in M0.0.

set "BACKEND=%~1"
if "%BACKEND%"=="" set "BACKEND=DX12"

rem wt-gate (2026-11-13): this bat builds the tree the SCRIPT LIVES IN
rem (%~dp0..), not cwd -- the #1 silent wrong-tree failure mode. Refuse to
rem run outside a registered ACTIVE worktree; mainline override is explicit:
rem   set NL_BUILD_MAINREPO=1   (or use scripts\wt.bat <track> build ...)
python "%~dp0..\tools\wt_gate.py" check-build "%~f0"
if errorlevel 1 exit /b 1

set "ROOT=%~dp0.."
set "MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.26100.0"
set "CMAKE_BIN=E:\UE_STUDY_2026\.build-tools\cmake-3.31.6-windows-x86_64\bin\cmake.exe"

set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%PATH%"

if not exist "%CMAKE_BIN%" set "CMAKE_BIN=cmake"

rem build\<backend> is reused so thirdparty stays incremental.
set "BUILDDIR=%ROOT%\build\%BACKEND%"

rem M-MOD0: subset build via NL_MODULES env var (semicolon list). Optional %2 =
rem build dir suffix (e.g. "subset" -> build\DX12-subset) to keep build\<backend>
rem cache in ALL state. Note: bat encoding must match the cmd ANSI codepage (GBK
rem on this machine) - UTF-8 Chinese comments get misparsed; this file is written
rem by AI tooling (always UTF-8) so it stays ASCII-only. Semicolon args also break
rem through bash/cmd quote stripping, so env var is the only subset channel.
set "MODULE_FLAGS="
if not "%NL_MODULES%"=="" set "MODULE_FLAGS=-DNL_MODULES=%NL_MODULES%"
set "BUILDDIR=%ROOT%\build\%BACKEND%"
if not "%~2"=="" set "BUILDDIR=%ROOT%\build\%BACKEND%-%~2"

echo === configure backend=%BACKEND% ===
"%CMAKE_BIN%" -S "%ROOT%" -B "%BUILDDIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=%BACKEND% %MODULE_FLAGS%
if errorlevel 1 exit /b 1

echo === build ===
"%CMAKE_BIN%" --build "%BUILDDIR%"
if errorlevel 1 exit /b 1

echo === selftest ===
"%BUILDDIR%\viewer.exe" --selftest
if errorlevel 1 exit /b 1

echo.
echo BUILD OK [%BACKEND%]: %BUILDDIR%\viewer.exe
exit /b 0

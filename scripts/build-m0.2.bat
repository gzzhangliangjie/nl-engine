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

echo === configure backend=%BACKEND% ===
"%CMAKE_BIN%" -S "%ROOT%" -B "%BUILDDIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=%BACKEND%
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

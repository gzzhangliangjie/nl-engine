@echo off
setlocal
rem Post-merge smoke: configure + build viewer only (no selftest; sandbox may not run viewer).
rem Usage: scripts\build-smoke-merge.bat DX12   (or VULKAN)
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

set "BUILDDIR=%ROOT%\build\%BACKEND%"

echo === configure backend=%BACKEND% ===
"%CMAKE_BIN%" -S "%ROOT%" -B "%BUILDDIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=%BACKEND%
if errorlevel 1 exit /b 1

echo === build viewer ===
"%CMAKE_BIN%" --build "%BUILDDIR%" --target viewer
if errorlevel 1 exit /b 1

echo SMOKE_BUILD_OK_%BACKEND%

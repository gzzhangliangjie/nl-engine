@echo off
setlocal
rem P8 M-S0: configure + build ONLY the audio-tools target for one backend tree.
rem A-track tool has zero graphics dependency; building just this target keeps the
rem loop fast while still using the project's standard NMake / explicit-SDK setup.
rem Usage: scripts\build-audio-tools.bat DX12   (or VULKAN)
rem vcvars64.bat is NOT used (reg.exe blocked by sandbox); see build-m0.2.bat.

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

set "BUILDDIR=%ROOT%\build\%BACKEND%"

echo === configure backend=%BACKEND% (audio-tools only) ===
"%CMAKE_BIN%" -S "%ROOT%" -B "%BUILDDIR%" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=%BACKEND%
if errorlevel 1 exit /b 1

echo === build audio-tools ===
"%CMAKE_BIN%" --build "%BUILDDIR%" --target audio-tools
if errorlevel 1 exit /b 1

echo === audio-tools selftest ===
"%BUILDDIR%\audio-tools.exe" selftest
if errorlevel 1 exit /b 1

echo BUILD OK [audio-tools %BACKEND%]: %BUILDDIR%\audio-tools.exe
exit /b 0

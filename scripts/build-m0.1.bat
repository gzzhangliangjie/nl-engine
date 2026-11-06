@echo off
setlocal enabledelayedexpansion
rem M0.1 one-shot build + acceptance. All output ASCII on purpose.

set "ROOT=%~dp0.."
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "LOCAL_CMAKE=E:\UE_STUDY_2026\.build-tools\cmake-3.31.6-windows-x86_64\bin\cmake.exe"

if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found: %VCVARS%
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] failed to initialize MSVC environment
    exit /b 1
)

set "CMAKE_BIN="
where cmake >nul 2>nul && set "CMAKE_BIN=cmake"
if not defined CMAKE_BIN if exist "%LOCAL_CMAKE%" set "CMAKE_BIN=%LOCAL_CMAKE%"
if not defined CMAKE_BIN (
    echo [ERROR] CMake not found. Install CMake 3.24+ or place it at:
    echo         %LOCAL_CMAKE%
    echo         M0.1 acceptance also works without CMake: run scripts\verify-m0.1-msvc.ps1
    exit /b 1
)

rem NMake generator calls cl.exe directly. The VS generator goes through MSBuild,
rem which throws when HTTPS_PROXY and https_proxy both exist (case-insensitive
rem Hashtable collision) -- see docs/MILESTONES.md M0.0 pitfall 6.
rem Optional: pass DX12 or VULKAN as first arg to switch backend. Neither needs a vendor SDK.
set "BACKEND=AUTO"
if not "%~1"=="" set "BACKEND=%~1"
echo === configure (backend=%BACKEND%) ===
"%CMAKE_BIN%" -S "%ROOT%" -B "%ROOT%\build\nmake" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=%BACKEND%
if errorlevel 1 exit /b 1

echo === build ===
"%CMAKE_BIN%" --build "%ROOT%\build\nmake"
if errorlevel 1 exit /b 1

echo === acceptance: selftest ===
"%ROOT%\build\nmake\viewer.exe" --selftest
if errorlevel 1 exit /b 1

echo === acceptance: ctest ===
"%CMAKE_BIN%" --test-dir "%ROOT%\build\nmake" --output-on-failure
if errorlevel 1 exit /b 1

echo.
echo M0.1 BUILD OK.
echo Visible case: run "%ROOT%\build\nmake\viewer.exe"
echo Watch the title bar frame counter, press ESC to quit, then save a screenshot to docs\screenshots\m0.1.png
exit /b 0

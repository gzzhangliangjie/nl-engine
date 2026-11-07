@echo off
rem M-A track: configure + build anim-tools (and nl_core) with NMake.
rem VS generator unusable here: HTTPS_PROXY/https_proxy case-collision crashes
rem DevShell/vcvars env import -- see docs/MILESTONES.md M0.0 pitfall 6.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64 failed
    exit /b 1
)
rem Sandbox blocks reg.exe so vcvars may fail to add the Windows SDK bin
rem (rc.exe / mt.exe) to PATH. Prepend it explicitly (SDK 10.0.26100.0).
set "PATH=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64;%PATH%"
rem vcvars registry lookups blocked -> INCLUDE/LIB may be incomplete too.
set "MSVC=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130"
set "SDK=C:\Program Files (x86)\Windows Kits\10"
set "INCLUDE=%MSVC%\include;%SDK%\Include\10.0.26100.0\ucrt;%SDK%\Include\10.0.26100.0\um;%SDK%\Include\10.0.26100.0\shared;%SDK%\Include\10.0.26100.0\winrt;%SDK%\Include\10.0.26100.0\cppwinrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\10.0.26100.0\um\x64;%SDK%\Lib\10.0.26100.0\ucrt\x64"
set "CMAKE=E:\UE_STUDY_2026\.build-tools\cmake-3.31.6-windows-x86_64\bin\cmake.exe"
"%CMAKE%" -S "%~dp0.." -B "%~dp0..\build\nmake" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=DX12
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%~dp0..\build\nmake" --target %*
exit /b %errorlevel%

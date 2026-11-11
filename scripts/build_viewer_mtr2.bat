@echo off
setlocal
REM M-TR2 编译验证专用：重配 + 编 viewer（不进 git，脚本仅本地用）
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
set INCLUDE=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\winrt
set LIB=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64
set PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130\bin\Hostx64\x64;C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64;%PATH%

cd /d E:\UE_STUDY_2026\nl-engine-profiling
if "%1"=="on" (
  echo === configure DX12-TRACY (NL_TRACY=ON) ===
  cmake -S . -B build/DX12-TRACY -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=DX12 -DNL_TRACY=ON
  cd /d E:\UE_STUDY_2026\nl-engine-profiling\build\DX12-TRACY
  echo === nmake viewer (TRACY) ===
  nmake viewer
  echo EXIT_TRACY=%ERRORLEVEL%
) else (
  echo === configure DX12 (OFF) ===
  cmake -S . -B build/DX12 -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNL_GRAPHICS_BACKEND=DX12
  cd /d E:\UE_STUDY_2026\nl-engine-profiling\build\DX12
  echo === nmake viewer (OFF) ===
  nmake viewer
  echo EXIT_OFF=%ERRORLEVEL%
)

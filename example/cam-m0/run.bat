@echo off
rem P8 M-CAM0 acceptance: camera family headless core (A-class camera track).
rem Layers:
rem   1. selftest on BOTH backend build trees (DX12 + VULKAN) - 6 sections
rem   2. cam-trace + cam-check (10 trace-derived anchors); free-trace + free-check (6)
rem   3. evidence PNG regen + byte-compare (fc /b) against committed baseline
rem   4. negative paths: no args (exit 2), unknown section (exit 2)
rem Exit 0 = all green. Build first:
rem   scripts\build-m0.2.bat DX12
rem   scripts\build-m0.2.bat VULKAN
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"

echo === selftest (DX12 build tree) ===
"%ROOT%\build\DX12\cam-tools.exe" selftest > "%D%selftest-dx12.log" 2>&1
if errorlevel 1 (
    type "%D%selftest-dx12.log"
    echo [ERROR] cam-tools selftest FAILED DX12
    exit /b 1
)
findstr /c:"selftest PASS (8 sections)" "%D%selftest-dx12.log" >nul || (echo [ERROR] missing PASS line & exit /b 1)
type "%D%selftest-dx12.log"

echo === selftest (VULKAN build tree) ===
"%ROOT%\build\VULKAN\cam-tools.exe" selftest > "%D%selftest-vulkan.log" 2>&1
if errorlevel 1 (
    type "%D%selftest-vulkan.log"
    echo [ERROR] cam-tools selftest FAILED VULKAN
    exit /b 1
)
findstr /c:"selftest PASS (8 sections)" "%D%selftest-vulkan.log" >nul || (echo [ERROR] missing PASS line & exit /b 1)

echo === cam-trace + cam-check (10 trace-derived anchors) ===
"%ROOT%\build\DX12\cam-tools.exe" cam-trace > "%D%cam-trace.tsv" 2>&1
if errorlevel 1 (echo [ERROR] cam-trace FAILED & exit /b 1)
"%ROOT%\build\DX12\cam-tools.exe" cam-check "%D%cam-trace.tsv" > "%D%cam-check.log" 2>&1
if errorlevel 1 (type "%D%cam-check.log" & echo [ERROR] cam-check FAILED & exit /b 1)
findstr /c:"cam-check PASS (150 rows, 10 anchors)" "%D%cam-check.log" >nul || (echo [ERROR] missing cam-check PASS & exit /b 1)
type "%D%cam-check.log"

echo === free-trace + free-check (6 trace-derived anchors) ===
"%ROOT%\build\DX12\cam-tools.exe" free-trace > "%D%free-trace.tsv" 2>&1
if errorlevel 1 (echo [ERROR] free-trace FAILED & exit /b 1)
"%ROOT%\build\DX12\cam-tools.exe" free-check "%D%free-trace.tsv" > "%D%free-check.log" 2>&1
if errorlevel 1 (type "%D%free-check.log" & echo [ERROR] free-check FAILED & exit /b 1)
findstr /c:"free-check PASS (60 rows, 6 anchors)" "%D%free-check.log" >nul || (echo [ERROR] missing free-check PASS & exit /b 1)
type "%D%free-check.log"

echo === evidence PNG regen + byte-compare ===
rem %D% ends with a backslash which would escape the closing quote; append a dot.
python "%ROOT%\tools\make-cam-m0-evidence.py" "%D%." "%D%acceptance-regen.png"
if errorlevel 1 (echo [ERROR] evidence regen FAILED & exit /b 1)
fc /b "%D%acceptance.png" "%D%acceptance-regen.png" >nul
if errorlevel 1 (echo [ERROR] evidence bytes differ from baseline & exit /b 1)
echo evidence bytes identical

echo === negative: no args (expect exit 2) ===
"%ROOT%\build\DX12\cam-tools.exe" > "%D%usage.log" 2>&1
if not errorlevel 2 (echo [ERROR] usage should exit 2 & exit /b 1)
echo usage exit 2 OK

echo === negative: unknown section (expect exit 2) ===
"%ROOT%\build\DX12\cam-tools.exe" selftest bogus > "%D%negative.log" 2>&1
if not errorlevel 2 (echo [ERROR] bogus section should exit 2 & exit /b 1)
findstr /c:"unknown selftest section" "%D%negative.log" >nul || (echo [ERROR] missing error text & exit /b 1)
echo negative exit 2 OK

echo M-CAM0 ACCEPTANCE ALL GREEN
endlocal

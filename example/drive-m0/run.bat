@echo off
rem P8 M-D0 acceptance: Jolt ContactListener -> Jolt-free POD contact event stream.
rem Five-stage gate; exit 0 = all green. Build first:
rem   scripts\build-m0.2.bat DX12
rem   scripts\build-m0.2.bat VULKAN
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"
set "TOOLS=%ROOT%\build\DX12\phys-tools.exe"
set "VTOOLS=%ROOT%\build\VULKAN\phys-tools.exe"

if not exist "%TOOLS%" (
    echo [ERROR] phys-tools not built: build\DX12\phys-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === [1/5] selftest --verbose (M-P0..M-P4 + j5c + contacts) ===
"%TOOLS%" selftest --verbose > "%D%selftest.log" 2>&1
if errorlevel 1 ( type "%D%selftest.log" & echo [ERROR] selftest FAILED & exit /b 1 )
findstr /c:"phys-d0 contacts: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing contacts anchor & exit /b 1)
findstr /c:"phys-j5c anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing j5c anchor & exit /b 1)
findstr /c:"phys-p4 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p4 anchor & exit /b 1)
findstr /c:"phys-p3 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p3 anchor & exit /b 1)
findstr /c:"phys-p2 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p2 anchor & exit /b 1)
findstr /c:"phys-p1 anchors: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p1 anchor & exit /b 1)
findstr /c:"phys-p0 selftest: PASS" "%D%selftest.log" >nul || (echo [ERROR] missing p0 anchor & exit /b 1)
type "%D%selftest.log"

echo === [2/5] contact-trace + contact-check (byte-stable dump) ===
copy /b "%D%contacts.tsv" "%D%contacts.base.tsv" >nul
"%TOOLS%" contact-trace --frames 240 --out "%D%contacts.tsv" > "%D%contact-trace.log" 2>&1
if errorlevel 1 ( type "%D%contact-trace.log" & echo [ERROR] contact-trace FAILED & exit /b 1 )
findstr /c:"wrote" "%D%contact-trace.log" >nul || (echo [ERROR] missing trace wrote line & exit /b 1)
fc /b "%D%contacts.tsv" "%D%contacts.base.tsv" >nul
if errorlevel 1 ( echo [ERROR] contacts.tsv is not byte-stable & exit /b 1 )
del "%D%contacts.base.tsv"
"%TOOLS%" contact-check "%D%contacts.tsv" > "%D%contact-check.log" 2>&1
if errorlevel 1 ( type "%D%contact-check.log" & echo [ERROR] contact-check FAILED & exit /b 1 )
findstr /c:"phys-d0 contact-check: PASS" "%D%contact-check.log" >nul || (echo [ERROR] missing check PASS line & exit /b 1)
type "%D%contact-trace.log"
type "%D%contact-check.log"

echo === [3/5] cross-backend DX12 vs VULKAN trace byte-identical ===
if exist "%VTOOLS%" (
    "%VTOOLS%" contact-trace --frames 240 --out "%D%contacts-vulkan.tsv" >nul
    if errorlevel 1 ( echo [ERROR] VULKAN contact-trace FAILED & exit /b 1 )
    fc /b "%D%contacts.tsv" "%D%contacts-vulkan.tsv" >nul
    if errorlevel 1 ( echo [ERROR] DX12 / VULKAN traces differ & exit /b 1 )
    del "%D%contacts-vulkan.tsv"
    echo DX12 vs VULKAN contact-trace byte-identical: OK > "%D%cross-backend.log"
) else (
    echo VULKAN build absent - cross-backend check SKIPPED > "%D%cross-backend.log"
)
type "%D%cross-backend.log"

echo === [4/5] usage / negative exit codes ===
"%TOOLS%" contact-trace --frames 0 > "%D%usage.log" 2>&1
if errorlevel 2 ( echo usage exit 2 OK >> "%D%usage.log" ) else ( type "%D%usage.log" & echo [ERROR] bad --frames should exit 2 & exit /b 1 )
"%TOOLS%" contact-check "%D%no-such-file.tsv" > "%D%negative.log" 2>&1
if errorlevel 1 ( echo negative exit 1 OK >> "%D%negative.log" ) else ( type "%D%negative.log" & echo [ERROR] missing file should exit 1 & exit /b 1 )
type "%D%usage.log"
type "%D%negative.log"

echo === [5/5] evidence PNG regenerate + byte-compare ===
copy /b "%D%events.png" "%D%events.base.png" >nul
copy /b "%D%acceptance.png" "%D%acceptance.base.png" >nul
python "%ROOT%\tools\make-drive-m0-evidence.py" || exit /b 1
fc /b "%D%events.png" "%D%events.base.png" >nul
if errorlevel 1 ( echo [ERROR] events.png is not byte-stable & exit /b 1 )
fc /b "%D%acceptance.png" "%D%acceptance.base.png" >nul
if errorlevel 1 ( echo [ERROR] acceptance.png is not byte-stable & exit /b 1 )
del "%D%events.base.png" "%D%acceptance.base.png"
echo evidence PNG byte-identical: OK

echo === DRIVE-M0 ACCEPTANCE: ALL GREEN ===
exit /b 0

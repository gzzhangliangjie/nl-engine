@echo off
rem M-SP0 acceptance: sprite core POD oracle + real TSV evidence PNGs. ASCII only.
rem Exit 0 = all green. Build first: scripts\wt.bat sprite2d build DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\sprite-tools.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] sprite-tools not built: build\dx12\sprite-tools.exe
    echo         build via scripts\wt.bat sprite2d build DX12
    exit /b 1
)

echo === selftest ===
"%EXE%" selftest > "%D%selftest.log" 2>&1
if errorlevel 1 (
    type "%D%selftest.log"
    echo [ERROR] sprite-tools selftest FAILED
    exit /b 1
)
type "%D%selftest.log"
findstr /c:"SELFTEST PASS (4/4)" "%D%selftest.log" >nul || (echo [ERROR] selftest anchor missing & exit /b 1)

echo === traces (atlas / flipbook / batch) ===
"%EXE%" atlas-trace --out "%D%atlas-trace.tsv" > "%D%atlas-trace.log" 2>&1
if errorlevel 1 (type "%D%atlas-trace.log" & echo [ERROR] atlas-trace FAILED & exit /b 1)
"%EXE%" flipbook-trace --out "%D%flipbook-trace.tsv" > "%D%flipbook-trace.log" 2>&1
if errorlevel 1 (type "%D%flipbook-trace.log" & echo [ERROR] flipbook-trace FAILED & exit /b 1)
"%EXE%" batch-trace --out "%D%batch-trace.tsv" > "%D%batch-trace.log" 2>&1
if errorlevel 1 (type "%D%batch-trace.log" & echo [ERROR] batch-trace FAILED & exit /b 1)
"%EXE%" atlas-check "%D%atlas-trace.tsv" > "%D%atlas-check.log" 2>&1
if errorlevel 1 (type "%D%atlas-check.log" & echo [ERROR] atlas-check FAILED & exit /b 1)
"%EXE%" flipbook-check "%D%flipbook-trace.tsv" > "%D%flipbook-check.log" 2>&1
if errorlevel 1 (type "%D%flipbook-check.log" & echo [ERROR] flipbook-check FAILED & exit /b 1)
"%EXE%" batch-check "%D%batch-trace.tsv" > "%D%batch-check.log" 2>&1
if errorlevel 1 (type "%D%batch-check.log" & echo [ERROR] batch-check FAILED & exit /b 1)
type "%D%atlas-check.log"
type "%D%flipbook-check.log"
type "%D%batch-check.log"

echo === determinism: batch-trace byte-identical across two runs ===
"%EXE%" batch-trace --out "%D%batch-b.tsv" >nul 2>&1
if errorlevel 1 (echo [ERROR] second batch-trace FAILED & exit /b 1)
fc /b "%D%batch-trace.tsv" "%D%batch-b.tsv" >nul || (echo [ERROR] determinism: TSVs differ & exit /b 1)
del "%D%batch-b.tsv"
echo determinism : batch-trace.tsv byte-identical

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    type "%D%usage.log"
    echo [ERROR] usage did not exit 2
    exit /b 1
)

echo === negative: missing --out (exit 2) and missing tsv (exit 1) ===
"%EXE%" atlas-trace --out > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative missing-out exit 2 OK
) else (
    echo [ERROR] missing-out did not exit 2
    exit /b 1
)
"%EXE%" atlas-check "%D%no-such-file.tsv" > "%D%negative-check.log" 2>&1
if errorlevel 1 (
    echo negative missing-file exit 1 OK
) else (
    echo [ERROR] missing-file did not fail
    exit /b 1
)

echo === evidence PNGs from the real logs / TSV ===
rem No path argument: %~dp0 ends in a backslash, and "%D%" would escape the quote.
rem The renderer resolves example/sprite2d/ from its own location by default.
python "%ROOT%\tools\make-sp0-evidence.py"
if errorlevel 1 (echo [ERROR] evidence renderer FAILED & exit /b 1)

echo ALL GREEN - M-SP0 sprite-tools acceptance
exit /b 0

@echo off
rem M-PQ0 acceptance: Jolt NarrowPhaseQuery six anchors + real TSV evidence PNGs. ASCII only.
rem Exit 0 = all green. Build first: scripts\wt.bat phys-query build DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\phys-tools.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] phys-tools not built: build\dx12\phys-tools.exe
    echo         build via scripts\wt.bat phys-query build DX12
    exit /b 1
)

echo === query-trace (raycast / sweep / overlap six anchors) ===
"%EXE%" query-trace --out "%D%query-trace.tsv" > "%D%query-trace.log" 2>&1
if errorlevel 1 (
    type "%D%query-trace.log"
    echo [ERROR] query-trace FAILED
    exit /b 1
)
type "%D%query-trace.log"

echo === query-check (independent oracle) ===
"%EXE%" query-check "%D%query-trace.tsv" > "%D%query-check.log" 2>&1
if errorlevel 1 (
    type "%D%query-check.log"
    echo [ERROR] query-check FAILED
    exit /b 1
)
type "%D%query-check.log"
findstr /c:"PASS (10 rows)" "%D%query-check.log" >nul || (echo [ERROR] query-check anchor missing & exit /b 1)

echo === determinism: query-trace byte-identical across two runs ===
"%EXE%" query-trace --out "%D%query-b.tsv" >nul 2>&1
if errorlevel 1 (echo [ERROR] second query-trace FAILED & exit /b 1)
fc /b "%D%query-trace.tsv" "%D%query-b.tsv" >nul || (echo [ERROR] determinism: TSVs differ & exit /b 1)
del "%D%query-b.tsv"
echo determinism : query-trace.tsv byte-identical

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
"%EXE%" query-trace --out > "%D%negative.log" 2>&1
if errorlevel 2 (
    echo negative missing-out exit 2 OK
) else (
    echo [ERROR] missing-out did not exit 2
    exit /b 1
)
"%EXE%" query-check "%D%no-such-file.tsv" > "%D%negative-check.log" 2>&1
if errorlevel 1 (
    echo negative missing-file exit 1 OK
) else (
    echo [ERROR] missing-file did not fail
    exit /b 1
)

echo === evidence PNGs from the real logs / TSV ===
rem No path argument: %~dp0 ends in a backslash, and "%D%" would escape the quote.
rem The renderer resolves example/phys-query/ from its own location by default.
python "%ROOT%\tools\make-pq0-evidence.py"
if errorlevel 1 (echo [ERROR] evidence renderer FAILED & exit /b 1)

echo ALL GREEN - M-PQ0 phys-tools acceptance
exit /b 0

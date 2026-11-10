@echo off
rem P8 M-I0 acceptance: core/input state layer (A-class input track).
rem Layers:
rem   1. selftest on BOTH backend build trees (DX12 + VULKAN) — 7 sections,
rem      edges/combos/repeat/mouse/oob/mods/determinism, numeric anchors only
rem   2. trace: dump the 12-frame synthetic drive sequence (real tool output)
rem      + check: re-verify the dumped TSV against the same 5 anchors
rem   3. evidence PNG regenerated from the TSV and byte-compared to the
rem      committed baseline (fc /b) — image must be a pure function of real output
rem   4. negative paths: unknown section (exit 2), missing tsv (exit 1)
rem Exit 0 = all green. Build first:
rem   scripts\build-m0.2.bat DX12
rem   scripts\build-m0.2.bat VULKAN
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"

echo === selftest (DX12 build tree) ===
"%ROOT%\build\DX12\input-tools.exe" selftest > "%D%selftest-dx12.log" 2>&1
if errorlevel 1 (
    type "%D%selftest-dx12.log"
    echo [ERROR] input-tools selftest FAILED DX12
    exit /b 1
)
findstr /c:"selftest PASS (7 sections)" "%D%selftest-dx12.log" >nul || (echo [ERROR] missing PASS line & exit /b 1)
type "%D%selftest-dx12.log"

echo === selftest (VULKAN build tree) ===
"%ROOT%\build\VULKAN\input-tools.exe" selftest > "%D%selftest-vulkan.log" 2>&1
if errorlevel 1 (
    type "%D%selftest-vulkan.log"
    echo [ERROR] input-tools selftest FAILED VULKAN
    exit /b 1
)
findstr /c:"selftest PASS (7 sections)" "%D%selftest-vulkan.log" >nul || (echo [ERROR] missing PASS line & exit /b 1)

echo === trace (real output) + check (5 anchors) ===
"%ROOT%\build\DX12\input-tools.exe" trace --frames 12 > "%D%input-trace.tsv" 2>&1
if errorlevel 1 (echo [ERROR] trace FAILED & exit /b 1)
"%ROOT%\build\DX12\input-tools.exe" check "%D%input-trace.tsv" > "%D%check.log" 2>&1
if errorlevel 1 (
    type "%D%check.log"
    echo [ERROR] check FAILED
    exit /b 1
)
findstr /c:"check PASS (12 rows, 5 anchors)" "%D%check.log" >nul || (echo [ERROR] missing check PASS line & exit /b 1)
type "%D%check.log"

echo === evidence PNG byte-compare (regenerate from TSV) ===
python "%ROOT%\tools\make-input-m0-evidence.py" "%D%input-trace.tsv" "%D%acceptance-regen.png" || exit /b 1
fc /b "%D%acceptance.png" "%D%acceptance-regen.png" >nul
if errorlevel 1 (
    echo [ERROR] evidence PNG is NOT byte-identical to committed baseline
    exit /b 1
)
echo acceptance-regen.png byte-identical to acceptance.png: OK

echo === negative: unknown selftest section (exit 2 expected) ===
"%ROOT%\build\DX12\input-tools.exe" selftest bogus > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    echo [ERROR] unknown section should exit 2
    exit /b 1
)
type "%D%usage.log"

echo === negative: check missing tsv (exit 1 expected) ===
"%ROOT%\build\DX12\input-tools.exe" check "%D%no-such-file.tsv" > "%D%negative.log" 2>&1
if errorlevel 1 if not errorlevel 2 (
    echo negative exit 1 OK
) else (
    echo [ERROR] missing tsv should exit 1
    exit /b 1
)
type "%D%negative.log"

echo === INPUT-M0 ACCEPTANCE: ALL GREEN ===
exit /b 0

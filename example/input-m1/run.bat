@rem input-m1 acceptance run: regenerate ALL evidence from the real tool, then
@rem machine-verify. 5 stages:
@rem   1. selftest on BOTH backend build trees (DX12 + VULKAN) - 9 sections,
@rem      grep the canonical PASS line
@rem   2. act-trace regeneration from the DX12 tree -> act-trace.tsv
@rem   3. act-check on the regenerated TSV (12 rows + 9 anchors)
@rem   4. evidence PNG regeneration via tools/make-input-m1-evidence.py
@rem   5. fc /b byte-compare regenerated PNG against the committed baseline
@rem Exit 0 = ALL GREEN.
@echo off
setlocal
set "ROOT=%~dp0..\.."
set "D=%~dp0"

"%ROOT%\build\DX12\input-tools.exe" selftest > "%D%selftest-dx12.log" 2>&1
if errorlevel 1 (echo [ERROR] input-tools selftest FAILED DX12 & exit /b 1)
findstr /c:"selftest PASS (9 sections)" "%D%selftest-dx12.log" >nul || (echo [ERROR] missing PASS line & exit /b 1)

"%ROOT%\build\VULKAN\input-tools.exe" selftest > "%D%selftest-vulkan.log" 2>&1
if errorlevel 1 (echo [ERROR] input-tools selftest FAILED VULKAN & exit /b 1)
findstr /c:"selftest PASS (9 sections)" "%D%selftest-vulkan.log" >nul || (echo [ERROR] missing PASS line & exit /b 1)

"%ROOT%\build\DX12\input-tools.exe" act-trace --frames 12 > "%D%act-trace.tsv"
if errorlevel 1 (echo [ERROR] act-trace FAILED & exit /b 1)

"%ROOT%\build\DX12\input-tools.exe" act-check "%D%act-trace.tsv" > "%D%act-check.log" 2>&1
if errorlevel 1 (echo [ERROR] act-check FAILED & exit /b 1)
findstr /c:"act-check PASS (12 rows, 9 anchors)" "%D%act-check.log" >nul || (echo [ERROR] missing act-check PASS line & exit /b 1)

python "%ROOT%\tools\make-input-m1-evidence.py" "%D%act-trace.tsv" "%D%acceptance.png" > "%D%evidence.log" 2>&1
if errorlevel 1 (echo [ERROR] evidence render FAILED & exit /b 1)

fc /b "%D%acceptance.png" "%D%acceptance.expected.png" >nul
if errorlevel 1 (echo [ERROR] PNG drift vs baseline & exit /b 1)

echo input-m1 ALL GREEN
exit /b 0

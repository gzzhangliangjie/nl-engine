@echo off
rem M-COV0 (cov-m0) verify: shader static coverage manifest gate.
rem Tool: tools/check-shader-coverage.py (parses CMakeLists artifact list,
rem runs slangc -trace-coverage -trace-coverage-boolean per artifact x dxil/spirv,
rem validates the compile-time <out>.coverage-manifest.json structure).
rem Verdict: compile error or invalid manifest -> exit 1 (FAIL gate).
rem Stages: 1) selftest  2) full gate (all artifacts x both targets)
rem         3) negative --only bogus (expect exit 2)  4) usage --target bogus (expect exit 2)
setlocal EnableExtensions
set "TOOL=%~dp0..\..\tools\check-shader-coverage.py"
set "OUT=%~dp0"

echo [1/4] selftest
python "%TOOL%" selftest > "%OUT%verify-selftest.log" 2>&1
if errorlevel 1 ( echo [FAIL] selftest nonzero exit & exit /b 1 )
findstr /c:"check-shader-coverage selftest: PASS" "%OUT%verify-selftest.log" >nul
if errorlevel 1 ( echo [FAIL] selftest anchor missing & exit /b 1 )
echo [OK] selftest PASS

echo [2/4] full gate (all artifacts x dxil+spirv)
python "%TOOL%" > "%OUT%verify-full.log" 2>&1
if errorlevel 1 ( echo [FAIL] full gate nonzero exit & exit /b 1 )
findstr /c:"check-shader-coverage: PASS" "%OUT%verify-full.log" >nul
if errorlevel 1 ( echo [FAIL] full gate anchor missing & exit /b 1 )
findstr /c:"manifests=" "%OUT%verify-full.log" >nul
if errorlevel 1 ( echo [FAIL] manifest count missing & exit /b 1 )
echo [OK] full gate PASS (log: verify-full.log)

echo [3/4] negative --only bogus (expect exit 2)
python "%TOOL%" --only bogus > "%OUT%verify-negative.log" 2>&1
if not errorlevel 1 ( echo [FAIL] negative case exited 0 & exit /b 1 )
findstr /c:"no artifact matches" "%OUT%verify-negative.log" >nul
if errorlevel 1 ( echo [FAIL] negative anchor missing & exit /b 1 )
echo [OK] negative exit 2 + anchor

echo [4/4] usage --target bogus (expect exit 2)
python "%TOOL%" --target bogus > "%OUT%verify-usage.log" 2>&1
if not errorlevel 1 ( echo [FAIL] usage case exited 0 & exit /b 1 )
findstr /c:"M-COV0" "%OUT%verify-usage.log" >nul
if errorlevel 1 ( echo [FAIL] usage anchor missing & exit /b 1 )
echo [OK] usage exit 2 + anchor

echo ALL GREEN [cov-m0]
endlocal

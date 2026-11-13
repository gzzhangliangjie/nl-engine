@echo off
rem M-PERF0 (perf-m0) verify: shader static uniformity gate.
rem Tool: tools/check-shader-uniformity.py (parses CMakeLists artifact list,
rem runs slangc -validate-uniformity per artifact x dxil/spirv).
rem Q-P3 verdict: uniformity warning or compile error -> exit 1 (FAIL gate);
rem other warnings (E30081 implicit conversion / E39001 binding overlap) are
rem informational only.
rem Stages: 1) selftest  2) full gate (all artifacts x both targets)
rem         3) negative --only bogus (expect exit 2)  4) usage --target bogus (expect exit 2)
setlocal EnableExtensions
set "TOOL=%~dp0..\..\tools\check-shader-uniformity.py"
set "OUT=%~dp0"

echo [1/4] selftest
python "%TOOL%" selftest > "%OUT%verify-selftest.log" 2>&1
if errorlevel 1 ( echo [FAIL] selftest nonzero exit & exit /b 1 )
findstr /c:"check-shader-uniformity selftest: PASS" "%OUT%verify-selftest.log" >nul
if errorlevel 1 ( echo [FAIL] selftest anchor missing & exit /b 1 )
echo [OK] selftest PASS

echo [2/4] full gate (all artifacts x dxil+spirv, ~13s)
python "%TOOL%" > "%OUT%verify-full.log" 2>&1
if errorlevel 1 ( echo [FAIL] full gate nonzero exit & exit /b 1 )
findstr /c:"check-shader-uniformity: PASS" "%OUT%verify-full.log" >nul
if errorlevel 1 ( echo [FAIL] full gate anchor missing & exit /b 1 )
findstr /c:"uniformity=0" "%OUT%verify-full.log" >nul
if errorlevel 1 ( echo [FAIL] uniformity count nonzero & exit /b 1 )
findstr /c:"compileErrors=0" "%OUT%verify-full.log" >nul
if errorlevel 1 ( echo [FAIL] compileErrors nonzero & exit /b 1 )
echo [OK] full gate PASS (uniformity=0 compileErrors=0, log: verify-full.log)

echo [3/4] negative --only bogus (expect exit 2)
python "%TOOL%" --only bogus > "%OUT%verify-negative.log" 2>&1
if not errorlevel 1 ( echo [FAIL] negative case exited 0 & exit /b 1 )
findstr /c:"no artifact matches" "%OUT%verify-negative.log" >nul
if errorlevel 1 ( echo [FAIL] negative anchor missing & exit /b 1 )
echo [OK] negative exit 2 + anchor

echo [4/4] usage --target bogus (expect exit 2)
python "%TOOL%" --target bogus > "%OUT%verify-usage.log" 2>&1
if not errorlevel 1 ( echo [FAIL] usage case exited 0 & exit /b 1 )
findstr /c:"M-PERF0" "%OUT%verify-usage.log" >nul
if errorlevel 1 ( echo [FAIL] usage anchor missing & exit /b 1 )
echo [OK] usage exit 2 + anchor

echo ALL GREEN [perf-m0]
endlocal

@echo off
setlocal
rem M14B2 (S14-B) restir baseline + statistical re-shot acceptance.
rem ASCII only; CRLF; no parentheses inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem restir is a stochastic path: Q-B1 semantics consult the S11 world_cache
rem indirect term, whose chain is run-to-run nondeterministic in restir mode
rem (S11 hashmap CAS insert races; measured 0.16 pct pixels / max 23/255
rem between fresh-process reruns of the same exe on dx12). With the cache
rem chain disabled by diagnostic bisect, two fresh runs are byte-identical.
rem Per the M14B2 acceptance record (option A) the re-shot gate is
rem statistical, calibrated with headroom over the measured spread:
rem   diff-pixel ratio <= 1.0 pct  (measured 0.16)
rem   mean abs diff    <= 0.5/255  (measured 0.005)
rem Assertions:
rem   - restir re-shot on both backends exits 0; dx12 deviceErrors=0
rem   - log anchor: restir: active (M=1 cosine candidate ...)
rem   - stability gate: re-shot vs frozen baseline (both backends)
rem   - present gate: baselines not degenerate
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --size 960x540 --yaw 12 --pitch 9 --dist 7 --frames 128 --no-hud"
set FAIL=0

echo [shot] re-shoot restir on both backends (dx12 log captured to verify-log.txt)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode restir %ARGS% --screenshot "%OUT%verify-restir-%%B.png" > "%OUT%verify-log.txt" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode restir %ARGS% --screenshot "%OUT%verify-restir-%%B.png" >nul 2>&1
  )
  if errorlevel 1 (
    echo [FAIL] restir-%%B viewer exited nonzero
    set FAIL=1
  )
)

findstr /C:"deviceErrors=0" "%OUT%verify-log.txt" <nul >nul
if errorlevel 1 (
    echo [FAIL] dx12 re-shot reported device errors
    set FAIL=1
) else (
    echo [OK] dx12 re-shot deviceErrors=0
)
findstr /C:"restir: active" "%OUT%verify-log.txt" <nul >nul
if errorlevel 1 (
    echo [FAIL] restir active anchor missing
    set FAIL=1
) else (
    echo [OK] restir: active anchor present
)

echo [check] stability gate: re-shot vs frozen baseline (statistical)
for %%B in (dx12 vulkan) do (
  python "%ROOT%\tools\check-s14b-restir.py" stability "%OUT%verify-restir-%%B.png" "%OUT%restir-%%B.png" 1.0 0.5
  if errorlevel 1 (
    echo [FAIL] restir-%%B stability gate failed
    set FAIL=1
  )
)

echo [check] present gate: baselines not degenerate
python "%ROOT%\tools\check-s14b-restir.py" present "%OUT%restir-dx12.png" 2.0 10
if errorlevel 1 (
    echo [FAIL] dx12 baseline present gate failed
    set FAIL=1
)
python "%ROOT%\tools\check-s14b-restir.py" present "%OUT%restir-vulkan.png" 2.0 10
if errorlevel 1 (
    echo [FAIL] vulkan baseline present gate failed
    set FAIL=1
)

echo [negative] missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode restir --model missing.nmesh --frames 1 --no-hud --screenshot "%OUT%verify-negative.png" >nul 2>&1
if errorlevel 1 (
    echo [OK] negative: missing model exits nonzero
) else (
    echo [FAIL] negative: missing model exited zero
    set FAIL=1
)

if "%FAIL%"=="1" (
    echo VERIFY FAILED
    exit /b 1
)
echo VERIFY OK
exit /b 0

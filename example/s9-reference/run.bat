@echo off
setlocal
rem M9.2 / S9 - offline reference mode (progressive multi-frame direct-light truth) acceptance shots.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M9.2 --mode reference: progressive multi-frame accumulation of 64 jittered shadow
rem rays/pixel via M8 TLAS; linear sum buffer (rtTarget_) divided by samplesDone then a
rem single gamma in present. This is the ground-truth direct-light + hard-shadow image
rem used to judge M9.1 shadow rays and VSM (S15). Assertions:
rem   - both backends render reference mode exit 0
rem   - re-shot == frozen baseline (deterministic, no regression)
rem   - log anchor: reference scene ready present; TLAS not ready ABSENT (rays dispatched)
rem   - progress reaches 100 percent (4096/4096 spp)
rem   - dx12 vs vulkan agree within tolerance (tool check-s9shadow.py near 0.01)
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --frames 64 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 9"
set FAIL=0

echo [shot] re-shoot reference on both backends (dx12 log captured to verify-log.txt for anchor checks)
for %%B in (dx12 vulkan) do (
  if "%%B"=="dx12" (
    "%ROOT%\build\%%B\viewer.exe" --mode reference %ARGS% --screenshot "%OUT%verify-reference-%%B.png" > "%OUT%verify-log.txt" 2>&1
  ) else (
    "%ROOT%\build\%%B\viewer.exe" --mode reference %ARGS% --screenshot "%OUT%verify-reference-%%B.png" >nul
  )
  if errorlevel 1 (
    echo [FAIL] reference-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] bytes: re-shot vs frozen baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%reference-%%B.png" "%OUT%verify-reference-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] reference-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] reference-%%B bytes identical to baseline
  )
)

echo [check] log anchor: reference scene ready + rays dispatched + 100 percent (from dx12 re-shot log)
findstr /C:"reference scene ready" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] reference scene ready log line missing
  set FAIL=1
) else (
  echo [OK] reference scene ready log line present
)
findstr /C:"TLAS not ready" "%OUT%verify-log.txt" >nul
if not errorlevel 1 (
  echo [FAIL] TLAS not ready - shadow rays disabled
  set FAIL=1
) else (
  echo [OK] TLAS ready - no TLAS not ready line
)
findstr /C:"4096 / 4096 spp (100%)" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] reference did not reach 100 percent accumulation
  set FAIL=1
) else (
  echo [OK] reference reached 100 percent accumulation
)

echo [check] dx12 vs vulkan within tolerance
python "%ROOT%\tools\check-s9shadow.py" near "%OUT%verify-reference-dx12.png" "%OUT%verify-reference-vulkan.png" 0.01
if errorlevel 1 (
  echo [FAIL] dx12/vulkan near gate failed
  set FAIL=1
) else (
  echo [OK] dx12/vulkan near gate passed
)

echo [check] negative: missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode reference --model NoSuchModel.nmesh --frames 64 --no-hud --size 320x240 > "%OUT%verify-negative.txt" 2>&1
if errorlevel 1 (
  echo [OK] negative exited nonzero
) else (
  echo [FAIL] negative silently accepted
  set FAIL=1
)

echo FINAL_FAIL=%FAIL%
if "%FAIL%"=="1" (
  echo VERIFY FAILED
  exit /b 1
)
echo VERIFY OK
exit /b 0

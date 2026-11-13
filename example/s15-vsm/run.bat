@echo off
setlocal
rem M15.0 / S15 - VSM (virtual shadow map, page table indirection) acceptance.
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M15.0 adds --vsm on --mode shadow: shadow factor source switches from the
rem M9.1 TLAS ray to a VSM page-table lookup + depth compare. What we assert:
rem   - both backends render --mode shadow --vsm exit 0, deviceErrors=0
rem   - re-shot --vsm == frozen baseline (deterministic, no regression)
rem     NOTE M15.1: baseline re-shot after the sampler chain moved to the
rem     4-level clipmap lookup (legal output change, noted in MILESTONES)
rem   - log anchor: "vsm clipmap live" present (M15.1 anchor)
rem   - degraded guard: WITHOUT --vsm the exe must stay byte-identical to the frozen
rem     s9-shadow baseline (ray path untouched, baseline zero disturbance)
rem   - dx12 vs vulkan agree within tolerance -- tool check-s9shadow.py near
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --shading 5 --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 9"
set FAIL=0

echo [shot] re-shoot vsm on both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode shadow %ARGS% --vsm --screenshot "%OUT%verify-vsm-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] vsm-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] bytes: re-shot vs frozen M15.0 baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%vsm-%%B.png" "%OUT%verify-vsm-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] vsm-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] vsm-%%B bytes identical to baseline
  )
)

echo [check] log anchor: vsm clipmap live
"%ROOT%\build\dx12\viewer.exe" --mode shadow %ARGS% --vsm --screenshot "%OUT%verify-log-dx12.png" > "%OUT%verify-log.txt" 2>&1
findstr /C:"vsm clipmap live" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] vsm clipmap log line missing
  set FAIL=1
) else (
  echo [OK] vsm clipmap log line present
)
findstr /C:"deviceErrors=0" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] device errors reported
  set FAIL=1
) else (
  echo [OK] deviceErrors=0
)

echo [check] degraded guard: no --vsm stays byte-identical to s9-shadow baseline
"%ROOT%\build\dx12\viewer.exe" --mode shadow %ARGS% --screenshot "%OUT%verify-base-dx12.png" >nul
fc /b "%ROOT%\example\s9-shadow\shadow-dx12.png" "%OUT%verify-base-dx12.png" >nul
if errorlevel 1 (
  echo [FAIL] ray path no longer matches s9-shadow baseline
  set FAIL=1
) else (
  echo [OK] no-vsm ray path byte-identical to s9-shadow baseline
)

echo [check] dx12 vs vulkan within tolerance
python "%ROOT%\tools\check-s9shadow.py" near "%OUT%vsm-dx12.png" "%OUT%vsm-vulkan.png" 0.01
if errorlevel 1 (
  echo [FAIL] dx12/vulkan near gate failed
  set FAIL=1
) else (
  echo [OK] dx12/vulkan near gate passed
)

echo [check] negative: missing model must exit nonzero
"%ROOT%\build\dx12\viewer.exe" --mode shadow --model NoSuchModel.nmesh --vsm --frames 8 --no-hud --size 320x240 > "%OUT%verify-negative.txt" 2>&1
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

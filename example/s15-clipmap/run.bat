@echo off
setlocal
rem M15.1 / S15 - VSM clipmap (4-level cascade + GPU page marking + CPU allocator).
rem ASCII only; CRLF; no parentheses or stray quotes inside echo text in blocks.
rem Usage: run.bat verify   exit 0 = all checks green.
rem
rem M15.1 upgrades the M15.0 single-shot VSM to a virtualized clipmap:
rem   - 4096x4096 R32F page pool (1024 pages of 128x128), 4 levels, virtual
rem     space 2048 per level, coarse window = 4x scene max extent
rem   - csMark compute pass marks requested pages from the visibility buffer
rem     (finest hit level only), 2-frame readback ring feeds a CPU allocator
rem     (cross-frame page cache + free list + LRU eviction)
rem   - page table rewritten on CPU each frame, dirty pages cleared via quad
rem What we assert:
rem   - both backends render --mode shadow --vsm exit 0, deviceErrors=0
rem   - re-shot --vsm == frozen M15.1 baseline (deterministic, no regression)
rem   - log anchors: "vsm clipmap live" + "cpu allocator" present
rem   - degraded guard: WITHOUT --vsm the exe must stay byte-identical to the frozen
rem     s9-shadow baseline (ray path untouched, baseline zero disturbance)
rem   - dx12 vs vulkan agree within tolerance -- tool check-s9shadow.py near 0.01
rem   - VSM vs ray baseline within near 0.02: the 128x128 texel pages +
rem     4-level cascade are an approximation of the exact ray. Baseline =
rem     --vsm-rayref shot (same csDirectVsm blob, ray-fallback path):
rem     comparing against the s9 frozen PNG would mix in blob drift
rem     (~4.7 pct, see S15-PLAN M15.1). 0.47 pct of pixels diverge
rem     (geometry-basis fallback-vs-fine + texel granularity), noted
rem   - negative: missing model exits nonzero

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set "ARGS=--model Suzanne-materials.nmesh --shading 5 --frames 8 --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 9"
set FAIL=0

echo [shot] re-shoot clipmap vsm on both backends
for %%B in (dx12 vulkan) do (
  "%ROOT%\build\%%B\viewer.exe" --mode shadow %ARGS% --vsm --screenshot "%OUT%verify-clipmap-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] clipmap-%%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] bytes: re-shot vs frozen M15.1 baseline
for %%B in (dx12 vulkan) do (
  fc /b "%OUT%clipmap-%%B.png" "%OUT%verify-clipmap-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] clipmap-%%B bytes differ from baseline
    set FAIL=1
  ) else (
    echo [OK] clipmap-%%B bytes identical to baseline
  )
)

echo [check] log anchors: vsm clipmap live + cpu allocator
"%ROOT%\build\dx12\viewer.exe" --mode shadow %ARGS% --vsm --screenshot "%OUT%verify-log-dx12.png" > "%OUT%verify-log.txt" 2>&1
findstr /C:"vsm clipmap live" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] vsm clipmap log line missing
  set FAIL=1
) else (
  echo [OK] vsm clipmap log line present
)
findstr /C:"cpu allocator" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] cpu allocator log token missing
  set FAIL=1
) else (
  echo [OK] cpu allocator log token present
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
python "%ROOT%\tools\check-s9shadow.py" near "%OUT%clipmap-dx12.png" "%OUT%clipmap-vulkan.png" 0.01
if errorlevel 1 (
  echo [FAIL] dx12/vulkan near gate failed
  set FAIL=1
) else (
  echo [OK] dx12/vulkan near gate passed
)

echo [shot] re-shoot rayref baseline (same blob, ray-fallback path)
"%ROOT%\build\dx12\viewer.exe" --mode shadow %ARGS% --vsm --vsm-rayref --screenshot "%OUT%verify-rayref-dx12.png" >nul
if errorlevel 1 (
  echo [FAIL] rayref viewer exited nonzero
  set FAIL=1
)

echo [check] vsm vs rayref ray baseline within approximation tolerance
python "%ROOT%\tools\check-s9shadow.py" near "%OUT%clipmap-dx12.png" "%OUT%verify-rayref-dx12.png" 0.02
if errorlevel 1 (
  echo [FAIL] vsm vs rayref baseline near gate failed
  set FAIL=1
) else (
  echo [OK] vsm vs rayref baseline near gate passed
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

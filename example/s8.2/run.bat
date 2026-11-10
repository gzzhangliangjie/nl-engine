@echo off
setlocal
rem M8.2 (S8.2) Ray Query debug ray closure acceptance shots.
rem ASCII only; !VAR! delayed expansion inside for blocks (M4.1 lesson).
rem Usage: run.bat verify   (re-shots rt-dist/rt-id pairs + spin/negative checks, exit 0 = ok)
rem
rem Evidence chain:
rem   - --mode rt: initRt -> initDag asset chain (BLAS 104 tri + TLAS 1 inst),
rem     compute csRt: one inline RayQuery per pixel from the camera (invViewProj
rem     unproject, TMin 0.001 / mask 0xFF, bevy trace_ray conventions).
rem   - --rt-debug dist: hit distance grayscale (near bright / far dark, hiz viz
rem     convention); --rt-debug id: instance hue + per-primitive brightness.
rem   - Depth-match evidence: rt traces the BLAS = fallback coarse mesh
rem     (104 tri, M8.0 design intent = coarse proxy) while hiz rasterizes
rem     the finest LOD at identical camera framing (docs/screenshots/s8.png).
rem   - Spin evidence: --tlas-spin rotates the TLAS instance 2.5 deg/frame; two
rem     runs with different --frames must produce DIFFERENT pixels (dynamic
rem     instance transform propagates into ray results), while the rasterized
rem     hiz image stays byte-identical (baseline untouched).
rem   - Negative: --rt-debug xyz rejected with exit 1 (never silently falls back).

set "ROOT=%~dp0..\.."
set "OUT=%~dp0"
set FAIL=0

for %%B in (dx12 vulkan) do (
  echo [shot] rt-dist on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode rt --hud --rt-debug dist --screenshot "%OUT%verify-rt-dist-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 (
    echo [FAIL] rt-dist %%B viewer exited nonzero
    set FAIL=1
  )
  echo [shot] rt-id on %%B ...
  "%ROOT%\build\%%B\viewer.exe" --mode rt --hud --rt-debug id --screenshot "%OUT%verify-rt-id-%%B.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 640x720
  if errorlevel 1 (
    echo [FAIL] rt-id %%B viewer exited nonzero
    set FAIL=1
  )
)

echo [check] bytes: verify shots vs baselines ...
for %%P in (rt-dist rt-id) do for %%B in (dx12 vulkan) do (
  fc /b "%OUT%%%P-%%B.png" "%OUT%verify-%%P-%%B.png" >nul
  if errorlevel 1 (
    echo [FAIL] %%P-%%B bytes differ
    set FAIL=1
  ) else (
    echo [OK] %%P-%%B bytes identical
  )
)

rem [check] rt scene ready log line (file redirect: pipe errorlevel is
rem ambiguous under cmd, viewer stdout must land in a file before findstr)
"%ROOT%\build\dx12\viewer.exe" --mode rt --screenshot "%OUT%verify-log-dx12.png" --frames 8 --yaw 12 --pitch 9 --dist 4.2 --size 320x240 > "%OUT%verify-log.txt" 2>&1
findstr /C:"rt scene ready" "%OUT%verify-log.txt" >nul
if errorlevel 1 (
  echo [FAIL] rt scene ready log line missing
  set FAIL=1
) else (
  echo [OK] rt scene ready log line present
)

echo [check] spin: different frame counts must give DIFFERENT pixels ...
"%ROOT%\build\dx12\viewer.exe" --mode rt --hud --tlas-spin --screenshot "%OUT%verify-spin-a.png" --frames 12 --yaw 12 --pitch 9 --dist 4.2 --size 320x240
if errorlevel 1 (
  echo [FAIL] spin-a viewer exited nonzero
  set FAIL=1
)
"%ROOT%\build\dx12\viewer.exe" --mode rt --hud --tlas-spin --screenshot "%OUT%verify-spin-b.png" --frames 200 --yaw 12 --pitch 9 --dist 4.2 --size 320x240
if errorlevel 1 (
  echo [FAIL] spin-b viewer exited nonzero
  set FAIL=1
)
fc /b "%OUT%verify-spin-a.png" "%OUT%verify-spin-b.png" >nul
if errorlevel 1 (
  echo [OK] spin images differ - dynamic TLAS transform visible to rays
) else (
  echo [FAIL] spin images identical
  set FAIL=1
)

echo [check] raster baseline untouched by spin (hiz 12 vs 200 frames identical) ...
"%ROOT%\build\dx12\viewer.exe" --mode hiz --tlas-spin --screenshot "%OUT%verify-hiz-a.png" --frames 12 --yaw 12 --pitch 9 --dist 4.2 --size 320x240
if errorlevel 1 (
  echo [FAIL] hiz-a viewer exited nonzero
  set FAIL=1
)
"%ROOT%\build\dx12\viewer.exe" --mode hiz --tlas-spin --screenshot "%OUT%verify-hiz-b.png" --frames 200 --yaw 12 --pitch 9 --dist 4.2 --size 320x240
if errorlevel 1 (
  echo [FAIL] hiz-b viewer exited nonzero
  set FAIL=1
)
fc /b "%OUT%verify-hiz-a.png" "%OUT%verify-hiz-b.png" >nul
if errorlevel 1 (
  echo [FAIL] hiz spin images differ - raster path polluted
  set FAIL=1
) else (
  echo [OK] hiz spin images identical
)

echo [check] negative: --rt-debug xyz must exit 1 ...
"%ROOT%\build\dx12\viewer.exe" --mode rt --rt-debug xyz >nul 2>&1
if errorlevel 1 (
  echo [OK] negative exit 1
) else (
  echo [FAIL] negative accepted
  set FAIL=1
)

if "%FAIL%"=="1" (
  echo VERIFY FAILED
  exit /b 1
)
echo VERIFY OK
exit /b 0

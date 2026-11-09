@echo off
rem P8 M-S1 acceptance: 3D attenuation sweep + concurrent mixing + clip cache
rem                     + provenance re-render check + evidence PNGs.
rem All logs are redirected from audio-tools real stdout (evidence generator consumes them).
rem Exit 0 = all green. Build first: scripts\build-audio-tools.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\audio-tools.exe"
set "WAVB=%ROOT%\assets\audio\test-tone-b.wav"
set "OUT=%~dp0spatial"
set "LOGS=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] audio-tools not built: build\dx12\audio-tools.exe
    echo         build via scripts\build-audio-tools.bat DX12
    exit /b 1
)
if not exist "%WAVB%" (
    echo [ERROR] second WAV missing: python scripts\make-audio-wav.py assets\audio\test-tone-b.wav 660 0.4
    exit /b 1
)

echo === usage (exit 2 expected) ===
"%EXE%" > "%LOGS%usage.log" 2>&1
if not errorlevel 2 (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%LOGS%usage.log"

echo === selftest (M-S0 21 + M-S1 12 = 33 assertions) ===
"%EXE%" selftest > "%LOGS%selftest.log" 2>&1
if errorlevel 1 (
    echo [ERROR] selftest failed
    type "%LOGS%selftest.log"
    exit /b 1
)
type "%LOGS%selftest.log"

echo === spatial (4 attenuation curves + mixing + clip cache) ===
"%EXE%" spatial "%OUT%" > "%LOGS%spatial.log" 2>&1
if errorlevel 1 (
    echo [ERROR] spatial failed
    type "%LOGS%spatial.log"
    exit /b 1
)
type "%LOGS%spatial.log"

echo === check (re-render ALL samples from provenance) ===
"%EXE%" check "%OUT%" > "%LOGS%check.log" 2>&1
if errorlevel 1 (
    echo [ERROR] check failed
    type "%LOGS%check.log"
    exit /b 1
)
type "%LOGS%check.log"

echo === probe second clip ===
"%EXE%" probe "%WAVB%" > "%LOGS%probe-b.log" 2>&1
if errorlevel 1 (
    echo [ERROR] probe-b failed
    type "%LOGS%probe-b.log"
    exit /b 1
)
type "%LOGS%probe-b.log"

echo === build negative fixtures from the REAL dump ===
python "%~dp0make-negative-fixtures.py"
if errorlevel 1 (
    echo [ERROR] negative fixture generation failed
    exit /b 1
)

echo === negative: tampered attenuation.tsv (exit 1 expected) ===
"%EXE%" check "%~dp0bad" > "%LOGS%negative-tsv.log" 2>&1
if errorlevel 2 (
    echo [ERROR] tampered tsv should exit 1, got a usage error
    exit /b 1
)
if not errorlevel 1 (
    echo [ERROR] tampered tsv should fail explicitly
    exit /b 1
)
type "%LOGS%negative-tsv.log"

echo === negative: tampered maxDistance provenance (exit 1 expected) ===
"%EXE%" check "%~dp0bad-prov" > "%LOGS%negative-prov.log" 2>&1
if errorlevel 2 (
    echo [ERROR] tampered provenance should exit 1, got a usage error
    exit /b 1
)
if not errorlevel 1 (
    echo [ERROR] tampered provenance should fail explicitly
    exit /b 1
)
type "%LOGS%negative-prov.log"

echo === evidence PNG regeneration (from real tsv / logs above) ===
python "%ROOT%\tools\make-audio-evidence.py" --s1
if errorlevel 1 (
    echo [ERROR] evidence generation failed
    exit /b 1
)

echo === AUDIO M-S1 ACCEPTANCE: ALL GREEN ===
exit /b 0

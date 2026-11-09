@echo off
rem P8 M-S0 acceptance: real offline PCM dump + provenance re-render check + evidence PNGs.
rem All logs are redirected from audio-tools real stdout (evidence generator consumes them).
rem Exit 0 = all green. Build first: scripts\build-audio-tools.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\audio-tools.exe"
set "WAV=%ROOT%\assets\audio\test-tone.wav"
set "OUT=%~dp0dump"
set "LOGS=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] audio-tools not built: build\dx12\audio-tools.exe
    echo         build via scripts\build-audio-tools.bat DX12
    exit /b 1
)
if not exist "%WAV%" (
    echo [ERROR] WAV missing: run python scripts\make-audio-wav.py
    exit /b 1
)

echo === usage (exit 2 expected) ===
"%EXE%" > "%LOGS%usage.log" 2>&1
if not errorlevel 2 (
    echo [ERROR] no-args should exit 2
    exit /b 1
)
type "%LOGS%usage.log"

echo === probe (positive) ===
"%EXE%" probe "%WAV%" > "%LOGS%probe.log" 2>&1
if errorlevel 1 (
    echo [ERROR] probe failed
    exit /b 1
)
type "%LOGS%probe.log"

echo === selftest (determinism + tamper rejection) ===
"%EXE%" selftest > "%LOGS%selftest.log" 2>&1
if errorlevel 1 (
    echo [ERROR] selftest failed
    type "%LOGS%selftest.log"
    exit /b 1
)
type "%LOGS%selftest.log"

echo === dump (real PCM -^> waveform.tsv + provenance.txt) ===
"%EXE%" dump "%WAV%" "%OUT%" > "%LOGS%dump.log" 2>&1
if errorlevel 1 (
    echo [ERROR] dump failed
    type "%LOGS%dump.log"
    exit /b 1
)
type "%LOGS%dump.log"

echo === check (re-render from provenance, per-sample compare) ===
"%EXE%" check "%OUT%" > "%LOGS%check.log" 2>&1
if errorlevel 1 (
    echo [ERROR] check failed
    type "%LOGS%check.log"
    exit /b 1
)
type "%LOGS%check.log"

echo === negative: missing WAV (exit 1 expected) ===
"%EXE%" probe "%WAV%.missing" > "%LOGS%negative.log" 2>&1
if not errorlevel 1 (
    echo [ERROR] missing WAV should fail explicitly
    exit /b 1
)
type "%LOGS%negative.log"

echo === negative: tampered provenance (exit 1 expected) ===
"%EXE%" check "%~dp0bad" > "%LOGS%negative-check.log" 2>&1
if not errorlevel 1 (
    echo [ERROR] tampered provenance should fail explicitly
    exit /b 1
)
type "%LOGS%negative-check.log"

echo === evidence PNG regeneration (from real logs / PCM above) ===
python "%ROOT%\tools\make-audio-evidence.py"
if errorlevel 1 (
    echo [ERROR] evidence generation failed
    exit /b 1
)

echo === AUDIO M-S0 ACCEPTANCE: ALL GREEN ===
exit /b 0

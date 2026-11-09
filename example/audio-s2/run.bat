@echo off
rem P8 M-S2 acceptance: game-internal audio debug (volume/solo/freeze/debug)
rem                     + FNV-1a64 anti-false-green hash verify + evidence PNGs.
rem All logs are redirected from audio-tools real stdout (evidence generator consumes them).
rem Exit 0 = all green. Build first: scripts\build-audio-tools.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\audio-tools.exe"
set "WAVA=%ROOT%\assets\audio\test-tone.wav"
set "WAVB=%ROOT%\assets\audio\test-tone-b.wav"
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

echo === selftest (M-S0 21 + M-S1 12 + M-S2 13 = 46 assertions) ===
"%EXE%" selftest > "%LOGS%selftest.log" 2>&1
if errorlevel 1 (
    echo [ERROR] selftest failed
    type "%LOGS%selftest.log"
    exit /b 1
)
type "%LOGS%selftest.log"

echo === volume 0.5 (peak halves; provenance records the debug state) ===
"%EXE%" volume 0.5 "%WAVA%" "%~dp0volume" > "%LOGS%volume.log" 2>&1
if errorlevel 1 (
    echo [ERROR] volume failed
    type "%LOGS%volume.log"
    exit /b 1
)
type "%LOGS%volume.log"

echo === solo 1 (source 1 passes, source 2 muted to gain 0) ===
"%EXE%" solo 1 "%WAVA%" "%WAVB%" "%~dp0solo" > "%LOGS%solo.log" 2>&1
if errorlevel 1 (
    echo [ERROR] solo failed
    type "%LOGS%solo.log"
    exit /b 1
)
type "%LOGS%solo.log"

echo === freeze (readFrames=0 while frozen, stepFrames advances one block) ===
"%EXE%" freeze "%WAVA%" "%~dp0freeze" > "%LOGS%freeze.log" 2>&1
if errorlevel 1 (
    echo [ERROR] freeze failed
    type "%LOGS%freeze.log"
    exit /b 1
)
type "%LOGS%freeze.log"

echo === debug (snapshot: entity/pos/dist/gain/state per source) ===
"%EXE%" debug "%WAVA%" "%WAVB%" "%~dp0debug" > "%LOGS%debug.log" 2>&1
if errorlevel 1 (
    echo [ERROR] debug failed
    type "%LOGS%debug.log"
    exit /b 1
)
type "%LOGS%debug.log"

echo === hash verify (anti-false-green, over all real waveform dumps) ===
python "%~dp0verify-dump-hash.py" "%~dp0volume" "%~dp0solo" "%~dp0freeze" > "%LOGS%verify-hash.log" 2>&1
if errorlevel 1 (
    echo [ERROR] hash verify failed
    type "%LOGS%verify-hash.log"
    exit /b 1
)
type "%LOGS%verify-hash.log"

echo === build negative fixtures from the REAL volume dump ===
python "%~dp0make-negative-fixtures.py"
if errorlevel 1 (
    echo [ERROR] negative fixture generation failed
    exit /b 1
)

echo === negative: tampered waveform sample (exit 1 expected) ===
python "%~dp0verify-dump-hash.py" "%~dp0bad-volume" > "%LOGS%negative-wave.log" 2>&1
if not errorlevel 1 (
    echo [ERROR] tampered waveform should fail explicitly
    exit /b 1
)
type "%LOGS%negative-wave.log"

echo === negative: tampered stored hash (exit 1 expected) ===
python "%~dp0verify-dump-hash.py" "%~dp0bad-hash" > "%LOGS%negative-hash.log" 2>&1
if not errorlevel 1 (
    echo [ERROR] tampered stored hash should fail explicitly
    exit /b 1
)
type "%LOGS%negative-hash.log"

echo === negative: solo missing wavB (exit 2 expected) ===
"%EXE%" solo 1 "%WAVA%" > "%LOGS%negative-usage.log" 2>&1
if not errorlevel 2 (
    echo [ERROR] missing arg should exit 2
    exit /b 1
)
type "%LOGS%negative-usage.log"

echo === evidence PNG regeneration (from real tsv / logs above) ===
python "%ROOT%\tools\make-audio-evidence.py" --s2
if errorlevel 1 (
    echo [ERROR] evidence generation failed
    exit /b 1
)

echo === AUDIO M-S2 ACCEPTANCE: ALL GREEN ===
exit /b 0

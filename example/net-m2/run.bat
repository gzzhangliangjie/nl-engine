@echo off
rem A-class NET M-NET2 acceptance: server-authoritative minimal closed loop.
rem session loopback + verify checker selftest + dual-process viewer run
rem (server 60Hz Jolt authority + client interp ghosts) + numeric anchor
rem (client interp vs server auth within one snapshot interval) + negatives.
rem ASCII only. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "TOOLS=%ROOT%\build\dx12\net-tools.exe"
set "VIEWER=%ROOT%\build\dx12\viewer.exe"
set "D=%~dp0"

if not exist "%TOOLS%" (
    echo [ERROR] net-tools not built: build\dx12\net-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)
if not exist "%VIEWER%" (
    echo [ERROR] viewer not built: build\dx12\viewer.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === session-loopback (handshake + input upload + snapshot interp bound) ===
"%TOOLS%" session-loopback <nul > "%D%session-loopback.log" 2>&1
if errorlevel 1 (type "%D%session-loopback.log" & echo [ERROR] session-loopback FAILED & exit /b 1)
findstr /c:"session-loopback : PASS" "%D%session-loopback.log" <nul >nul || (echo [ERROR] PASS anchor missing & exit /b 1)
type "%D%session-loopback.log"

echo === verify-m2 selftest (checker sensitivity: exact lerp passes, +0.5m fails) ===
"%TOOLS%" verify-m2 --selftest <nul > "%D%verify-selftest.log" 2>&1
if errorlevel 1 (type "%D%verify-selftest.log" & echo [ERROR] verify selftest FAILED & exit /b 1)
findstr /c:"verify-m2 selftest: PASS" "%D%verify-selftest.log" <nul >nul || (echo [ERROR] PASS anchor missing & exit /b 1)
type "%D%verify-selftest.log"

echo === dual-process: viewer server + viewer client (scenario-2 training ground) ===
rem Server first (authoritative 60Hz Jolt), client joins via ENet connect retry.
rem Server 800 frames finishes while the client (900 frames) is still connected,
rem so both evidence PNGs contain both vehicles. --hud shows the Net Status
rem window (evidence only, not MD5-baselined: packet arrival is real-time).
rem Screens are aligned via fixed orbit framing (yaw 0 / pitch 35 / dist 26).
del "%D%server.log" "%D%client.log" "%D%server.png" "%D%client.png" 2>nul
start /b "" "%VIEWER%" --net-server 29216 --physics-debug --physics-scenario 2 --drive-demo --yaw 0 --pitch 35 --dist 26 --hud --screenshot "%D%server.png" --frames 800 --fixed-dt 16.666 > "%D%server.log" 2>&1
ping -n 2 127.0.0.1 >nul
"%VIEWER%" --net-client 127.0.0.1:29216 --physics-debug --physics-scenario 2 --drive-demo --yaw 0 --pitch 35 --dist 26 --hud --screenshot "%D%client.png" --frames 900 --fixed-dt 16.666 <nul > "%D%client.log" 2>&1
if errorlevel 1 (type "%D%client.log" & echo [ERROR] client viewer FAILED & exit /b 1)
findstr /c:"net server: listening" "%D%server.log" <nul >nul || (echo [ERROR] server listening anchor missing & exit /b 1)
findstr /c:"entity 2 joined" "%D%server.log" <nul >nul || (echo [ERROR] join anchor missing & type "%D%server.log" & exit /b 1)
findstr /c:"ghost slot" "%D%client.log" <nul >nul || (echo [ERROR] client ghost anchor missing & type "%D%client.log" & exit /b 1)
findstr /c:"interp tick" "%D%client.log" <nul >nul || (echo [ERROR] interp evidence missing & type "%D%client.log" & exit /b 1)
echo client: joined + interp evidence OK

echo === numeric anchor: client interp vs server auth (one snapshot interval) ===
"%TOOLS%" verify-m2 --auth "%D%server.log" --interp "%D%client.log" --min-samples 200 <nul > "%D%verify-pass.log" 2>&1
if errorlevel 1 (type "%D%verify-pass.log" & echo [ERROR] numeric anchor FAILED & exit /b 1)
findstr /c:"verify-m2    : PASS" "%D%verify-pass.log" <nul >nul || (echo [ERROR] PASS anchor missing & exit /b 1)
type "%D%verify-pass.log"

echo === negative: interp log without auth samples must FAIL explicitly (exit 1) ===
"%TOOLS%" verify-m2 --auth "%D%server.log" --interp "%D%server.log" --min-samples 1 <nul > "%D%verify-negative.log" 2>&1
if not errorlevel 1 (echo [ERROR] mismatched logs should exit 1 & exit /b 1)
findstr /c:"verify-m2    : FAIL" "%D%verify-negative.log" <nul >nul || (echo [ERROR] explicit FAIL anchor missing & exit /b 1)
type "%D%verify-negative.log"

echo === wait server shutdown (self-terminates after 800 frames) ===
set /a tries=0
:waitserver
findstr /c:"shutdown ok" "%D%server.log" <nul >nul && goto serverok
set /a tries+=1
if %tries% gtr 60 (echo [ERROR] server shutdown missing & exit /b 1)
ping -n 2 127.0.0.1 >nul
goto waitserver
:serverok
findstr /c:"entity 2 left" "%D%server.log" <nul >nul && echo note: client left before server end (entity recycled)
rem M-NET3: server graceful close (per-peer disconnect + flush) makes the client
rem detect link loss immediately when the server exits first. Exit order is
rem real-time dependent, so this is a soft note, not a hard anchor. The
rem deterministic anchor is session-loopback passiveClose (Sync layer, ctest).
findstr /c:"client: server disconnected" "%D%client.log" <nul >nul && echo note: client saw server disconnect (graceful close, ghosts removed)
echo server: shutdown ok

echo.
echo ALL GREEN: net-m2 acceptance
exit /b 0

@echo off
rem A-class NET M-NET0 acceptance: vendored ENet 1.3.18 + nl/net thin wrapper + echo tools.
rem Single-process loopback (echo) + dual-process (echo-server/echo-client) + negatives.
rem ASCII only. Exit 0 = all green. Build first: scripts\build-m0.2.bat DX12
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\net-tools.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] net-tools not built: build\dx12\net-tools.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === version anchor (vendored traceability) ===
"%EXE%" version <nul > "%D%version.log" 2>&1
if errorlevel 1 (echo [ERROR] version FAILED & exit /b 1)
findstr /c:"enet        : 1.3.18" "%D%version.log" <nul >nul || (echo [ERROR] version anchor missing & exit /b 1)
type "%D%version.log"

echo === usage (expect exit 2 = usage error, nav-tools convention) ===
"%EXE%" <nul > "%D%usage.log" 2>&1
if not errorlevel 1 (echo [ERROR] usage should exit 2 & exit /b 1)
findstr /c:"net-tools - nl-engine transport acceptance tool" "%D%usage.log" <nul >nul || (echo [ERROR] usage header missing & exit /b 1)
type "%D%usage.log"

echo === single-process loopback echo (reliable 32 + unreliable 64, all echoed) ===
"%EXE%" echo <nul > "%D%echo.log" 2>&1
if errorlevel 1 (type "%D%echo.log" & echo [ERROR] echo FAILED & exit /b 1)
findstr /c:"reliable    : 32/32" "%D%echo.log" <nul >nul || (echo [ERROR] reliable counter missing & exit /b 1)
findstr /c:"unreliable  : 64/64" "%D%echo.log" <nul >nul || (echo [ERROR] unreliable counter missing & exit /b 1)
findstr /c:"echo        : OK" "%D%echo.log" <nul >nul || (echo [ERROR] OK anchor missing & exit /b 1)
type "%D%echo.log"

echo === selftest (deterministic loopback assertions) ===
"%EXE%" selftest <nul > "%D%selftest.log" 2>&1
if errorlevel 1 (type "%D%selftest.log" & echo [ERROR] selftest FAILED & exit /b 1)
findstr /c:"selftest net: PASS" "%D%selftest.log" <nul >nul || (echo [ERROR] PASS anchor missing & exit /b 1)
type "%D%selftest.log"

echo === dual-process: echo-server + echo-client (32 reliable round trips) ===
rem start /b direct exe (no intermediate cmd): child inherits redirected stdout;
rem `start /min cmd /c` dies in sandboxed sessions (child reaped) - do not regress.
del "%D%server.log" 2>nul
start /b "" "%EXE%" echo-server 29192 --packets 32 --timeout-ms 20000 > "%D%server.log" 2>&1
ping -n 2 127.0.0.1 >nul
"%EXE%" echo-client 127.0.0.1 29192 --packets 32 --rate 100 --timeout-ms 10000 <nul > "%D%client.log" 2>&1
if errorlevel 1 (type "%D%client.log" & echo [ERROR] dual-process echo FAILED & exit /b 1)
findstr /c:"sent=32 recv=32 loss=0.0%%" "%D%client.log" <nul >nul || (echo [ERROR] client zero-loss anchor missing & exit /b 1)
type "%D%client.log"
rem server exits after serving 32; poll for its summary (up to ~30s)
set /a tries=0
:waitserver
if exist "%D%server.log" findstr /c:"echo-server : served=32" "%D%server.log" <nul >nul && goto serverok
set /a tries+=1
if %tries% gtr 30 (echo [ERROR] server summary missing & type "%D%server.log" & exit /b 1)
ping -n 2 127.0.0.1 >nul
goto waitserver
:serverok
type "%D%server.log"

echo === dual-process with 30%% inbound loss injection (deterministic seed 1234) ===
rem server must outlive the client deadline (--linger-ms): dropped echoes have no
rem resend, client hits its own timeout and prints the loss summary (exit 2).
del "%D%server-loss.log" 2>nul
start /b "" "%EXE%" echo-server 29193 --packets 32 --linger-ms 15000 --timeout-ms 25000 > "%D%server-loss.log" 2>&1
ping -n 2 127.0.0.1 >nul
"%EXE%" echo-client 127.0.0.1 29193 --packets 32 --rate 100 --loss 30 --timeout-ms 4000 <nul > "%D%client-loss.log" 2>&1
if not errorlevel 1 (echo [ERROR] loss run should exit 2 & exit /b 1)
findstr /c:"sent=32" "%D%client-loss.log" <nul >nul || (echo [ERROR] loss sent anchor missing & exit /b 1)
findstr /c:"loss=" "%D%client-loss.log" <nul >nul || (echo [ERROR] loss anchor missing & exit /b 1)
type "%D%client-loss.log"

echo === negative: connect to dead port must fail explicitly (exit 1) ===
"%EXE%" echo-client 127.0.0.1 29199 --packets 4 --timeout-ms 1500 <nul > "%D%negative.log" 2>&1
if not errorlevel 1 (echo [ERROR] dead port should exit 1 & exit /b 1)
findstr /c:"connect     : FAILED" "%D%negative.log" <nul >nul || (echo [ERROR] explicit reason missing & exit /b 1)
type "%D%negative.log"

echo.
echo ALL GREEN: net-m0 acceptance
exit /b 0

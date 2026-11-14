@echo off
rem M-SC4 (C# script track) acceptance: binding export generator -- gen.py golden check
rem + regenerate + drift-gate asserts excerpt + full selftest (legacy 16 exports replayed
rem through generated wiring, incl. hot-reload s8 after the M-GL4 fix), then evidence PNG
rem regeneration from real output. Exit 0 = all green.
rem Requires: build\DX12\nl-script.exe and dotnet SDK on PATH.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\nl-script.exe"
set "LOGS=%~dp0"
set "HELLO=%ROOT%\script\examples\hello"
set "CONS=%ROOT%\script\examples\console"
set "ECSP=%ROOT%\script\examples\ecs"
set "BOOT=%ROOT%\script\bootstrap"

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\DX12\nl-script.exe
    echo         build via scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === gen.py --check (golden byte-compare, exit 0 expected) ===
python "%ROOT%\tools\scriptapi\gen.py" --check > "%LOGS%gen-check.log" || exit /b 1
type "%LOGS%gen-check.log"

echo === gen.py regenerate (writes 4 artifacts from exports.api) ===
python "%ROOT%\tools\scriptapi\gen.py" > "%LOGS%gen-build.log" || exit /b 1
type "%LOGS%gen-build.log"

echo === ApiTable.g.asserts.inc excerpt (compile-time drift gate, generated file) ===
python -c "import io; ls=io.open(r'%ROOT%\core\src\script\ApiTable.g.asserts.inc',encoding='utf-8').read().splitlines(); n=len([l for l in ls if 'static_assert' in l]); io.open(r'%LOGS%asserts.log','w',encoding='utf-8',newline='\n').write('\n'.join(ls[:34])+'\n    ... ('+str(n)+' static_asserts total)\n')" || exit /b 1
type "%LOGS%asserts.log"

echo === selftest full (s0-s5 hosting + s6 console + s7 ECS + s8 hot reload) ===
"%EXE%" selftest "%HELLO%" "%CONS%" "%ECSP%" "%BOOT%" > "%LOGS%selftest.log" || exit /b 1
type "%LOGS%selftest.log"

echo === evidence PNG regeneration (from real logs above) ===
python "%ROOT%\tools\make-script-evidence.py" m-sc4 || exit /b 1

echo === M-SC4 ACCEPTANCE: ALL GREEN ===
exit /b 0

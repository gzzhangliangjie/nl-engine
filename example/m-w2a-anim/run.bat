@echo off
rem M-W2A anim domain acceptance: nl-script selftest s16+s17+s18 full 14-dir run.
rem ASCII only. Exit 0 = all green. Build first: build\dx12\nl-script.exe.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\dx12\nl-script.exe"
set "D=%~dp0"

if not exist "%EXE%" (
    echo [ERROR] nl-script not built: build\dx12\nl-script.exe
    echo         build the main tree first: NL_BUILD_MAINREPO=1 scripts\build-m0.2.bat DX12
    exit /b 1
)

echo === usage (exit 2 expected) ===
"%EXE%" > "%D%usage.log" 2>&1
if errorlevel 2 (
    echo usage exit 2 OK
) else (
    type "%D%usage.log"
    echo [ERROR] usage did not exit 2
    exit /b 1
)

echo === positive: selftest s18 anim domain (full 14-dir run, hook section included) ===
pushd "%ROOT%"
"%EXE%" selftest script/examples/hello script/examples/console script/examples/ecs script/bootstrap script/examples/timeinput script/examples/camera script/examples/model script/examples/drive script/examples/props script/examples/mouse script/examples/gamepad script/examples/audio script/examples/query script/examples/anim > "%D%anim.log" 2>&1
set "RC=%errorlevel%"
popd
if not "%RC%"=="0" (
    type "%D%anim.log"
    echo [ERROR] anim selftest FAILED rc=%RC%
    exit /b 1
)
findstr /c:"nl-script selftest: PASS (0 failures)" "%D%anim.log" >nul || (echo [ERROR] anim PASS anchor missing & exit /b 1)
echo anim selftest PASS anchor found

echo === negative: nonexistent project dir (dotnet build fails, exit 1) ===
"%EXE%" selftest "%D%no-such-dir" > "%D%negative.log" 2>&1
if errorlevel 1 (
    echo negative missing-dir exit 1 OK
) else (
    echo [ERROR] missing-dir did not fail
    exit /b 1
)

echo === evidence PNGs from the real logs ===
python "%ROOT%\tools\make-w2a-evidence.py" anim
if errorlevel 1 (echo [ERROR] evidence renderer FAILED & exit /b 1)

echo ALL GREEN - M-W2A anim domain acceptance
exit /b 0

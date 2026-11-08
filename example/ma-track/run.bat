@echo off
rem M-A track (action system) acceptance: deterministic selftests + sample dump
rem + evidence PNG regeneration. All output ASCII. Exit 0 = all green.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\nmake\anim-tools.exe"

if not exist "%EXE%" (
    echo [ERROR] anim-tools not built: build\nmake\anim-tools.exe
    echo         build via scripts\build-anim.bat anim-tools
    exit /b 1
)

echo === selftest curve ===
"%EXE%" selftest curve || exit /b 1
echo === selftest blend ===
"%EXE%" selftest blend || exit /b 1
echo === selftest statemachine ===
"%EXE%" selftest statemachine || exit /b 1
echo === selftest blendspace ===
"%EXE%" selftest blendspace || exit /b 1
echo === selftest skeleton (rigged glTF + FK) ===
python "%ROOT%\tools\make_rigged_glb.py" "%ROOT%\assets\models\RiggedChain.glb" || exit /b 1
"%EXE%" selftest skeleton "%ROOT%\assets\models\RiggedChain.glb" || exit /b 1
echo === selftest skinning (M-A5 LBS + rigged asset end-to-end) ===
"%EXE%" selftest skinning "%ROOT%\assets\models\rigged\CesiumMan.glb" || exit /b 1
echo === standard rigged assets (Khronos glTF-Sample-Assets) ===
"%EXE%" inspect "%ROOT%\assets\models\rigged\Fox.glb" | findstr /C:"bones 26 clips 3" >nul || exit /b 1
"%EXE%" inspect "%ROOT%\assets\models\rigged\CesiumMan.glb" | findstr /C:"bones 22 clips 1" >nul || exit /b 1
"%EXE%" inspect "%ROOT%\assets\models\rigged\RiggedFigure.glb" | findstr /C:"bones 22 clips 1" >nul || exit /b 1
echo === fkdump Fox.glb walk (deterministic demo data) ===
"%EXE%" fkdump "%ROOT%\assets\models\rigged\Fox.glb" 1 8 | findstr /C:"# fkdump" || exit /b 1
echo === negative: unknown case (expect exit 2) ===
"%EXE%" selftest bogus >nul 2>&1
if not errorlevel 2 (
    echo [ERROR] unknown selftest case should exit 2
    exit /b 1
)
echo === sample dump + evidence PNG ===
"%EXE%" sample || exit /b 1
python "%ROOT%\tools\anim_evidence.py" || exit /b 1

echo === MA-TRACK ACCEPTANCE: ALL GREEN ===
exit /b 0

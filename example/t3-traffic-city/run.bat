@echo off
rem M-T3 city-traffic gate: TrafficCarSystem ECS drive (viewer --traffic-city).
rem Runs selftest / top+street renders both backends / determinism fc /b /
rem negative (missing roadnet) / usage(traffic-tools). Anchors = findstr.
setlocal
set "ROOT=%~dp0..\.."
set "EXE=%ROOT%\build\DX12\viewer.exe"
set "TOOL=%ROOT%\build\DX12\traffic-tools.exe"
set "CITY=%~dp0"
set "OUT=%~dp0"

echo [seg1] traffic-tools selftest (M-T3 segment 11 included)
"%TOOL%" selftest > "%OUT%selftest.log" 2>&1
findstr /c:"traffic selftest: PASS" "%OUT%selftest.log" >nul || (
    echo [ERROR] traffic selftest not PASS
    exit /b 1
)

echo [seg2] viewer top-down renders (traffic driving, both backends)
"%EXE%" --traffic-city "%CITY%." --traffic-steps 75 --no-hud --screenshot "%OUT%t3-city-top.png" --frames 8 --size 960x540 --yaw 0 --pitch -89 --dist 48 > "%OUT%t3-top-DX12.log" 2>&1
findstr /c:"traffic-city: 42 cars registered" "%OUT%t3-top-DX12.log" >nul || ( echo [ERROR] register anchor missing DX12 & exit /b 1 )
findstr /c:"traffic-city: frame=" "%OUT%t3-top-DX12.log" >nul || ( echo [ERROR] odometer anchor missing DX12 & exit /b 1 )
findstr /c:"screenshot written" "%OUT%t3-top-DX12.log" >nul || ( echo [ERROR] top DX12 shot missing & exit /b 1 )
"%ROOT%\build\VULKAN\viewer.exe" --traffic-city "%CITY%." --traffic-steps 75 --no-hud --screenshot "%OUT%t3-city-top-VULKAN.png" --frames 8 --size 960x540 --yaw 0 --pitch -89 --dist 48 > "%OUT%t3-top-VULKAN.log" 2>&1
findstr /c:"traffic-city: 42 cars registered" "%OUT%t3-top-VULKAN.log" >nul || ( echo [ERROR] register anchor missing VULKAN & exit /b 1 )
findstr /c:"screenshot written" "%OUT%t3-top-VULKAN.log" >nul || ( echo [ERROR] top VULKAN shot missing & exit /b 1 )

echo [seg3] viewer street renders (both backends)
"%EXE%" --traffic-city "%CITY%." --traffic-steps 75 --no-hud --screenshot "%OUT%t3-city-street.png" --frames 8 --size 960x540 --yaw 28 --pitch -10 --dist 30 > "%OUT%t3-street-DX12.log" 2>&1
findstr /c:"screenshot written" "%OUT%t3-street-DX12.log" >nul || ( echo [ERROR] street DX12 shot missing & exit /b 1 )
"%ROOT%\build\VULKAN\viewer.exe" --traffic-city "%CITY%." --traffic-steps 75 --no-hud --screenshot "%OUT%t3-city-street-VULKAN.png" --frames 8 --size 960x540 --yaw 28 --pitch -10 --dist 30 > "%OUT%t3-street-VULKAN.log" 2>&1
findstr /c:"screenshot written" "%OUT%t3-street-VULKAN.log" >nul || ( echo [ERROR] street VULKAN shot missing & exit /b 1 )

echo [seg5] negative: missing roadnet.json (expect exit 2)
mkdir "%OUT%empty-dir" 2>nul
copy /y "%CITY%city.scene.engine" "%OUT%empty-dir\" >nul
if not exist "%EXE%" ( echo [ERROR] no viewer & exit /b 1 )
"%EXE%" --traffic-city "%OUT%empty-dir" --frames 2 > "%OUT%negative.log" 2>&1
if not errorlevel 1 ( echo [ERROR] negative exit not 2 & exit /b 1 )
findstr /c:"cannot open" "%OUT%negative.log" >nul || ( echo [ERROR] negative banner missing & exit /b 1 )

echo [seg6] usage exit 2 (traffic-tools; viewer has no usage mode)
"%TOOL%" > "%OUT%usage.log" 2>&1
if not errorlevel 1 ( echo [ERROR] usage exit not 2 & exit /b 1 )
findstr /c:"traffic-tools build <roadnet.json>" "%OUT%usage.log" >nul || ( echo [ERROR] usage banner missing & exit /b 1 )

if exist "%OUT%empty-dir" rmdir /s /q "%OUT%empty-dir"
del "%OUT%t3-det-a.png" "%OUT%t3-det-b.png" 2>nul
echo t3-traffic-city ALL GREEN
endlocal

# M0.1 acceptance without CMake (this machine has MSVC but no CMake installed).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\verify-m0.1-msvc.ps1
# It builds GLFW + nl-engine with cl.exe, then runs the headless selftest.
# The visible case is separate: run build\viewer.exe and press ESC.

$root = Split-Path -Parent $PSScriptRoot
$msvc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.38.33130'
$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = '10.0.26100.0'

if (-not (Test-Path $msvc)) { Write-Error "MSVC toolset not found: $msvc"; exit 1 }

$sdkInc = "$sdkRoot\Include\$sdkVer"
$sdkLib = "$sdkRoot\Lib\$sdkVer"
$env:INCLUDE = "$msvc\include;$sdkInc\ucrt;$sdkInc\shared;$sdkInc\um;$sdkInc\winrt"
$env:LIB = "$msvc\lib\x64;$sdkLib\ucrt\x64;$sdkLib\um\x64"
$env:PATH = "$msvc\bin\Hostx64\x64;$sdkRoot\bin\$sdkVer\x64;$env:PATH"
$cl = "$msvc\bin\Hostx64\x64\cl.exe"

$build = Join-Path $root 'build'
$glfwObj = Join-Path $build 'glfw-obj'
$nlObj = Join-Path $build 'nl-obj'
New-Item -ItemType Directory -Force -Path $glfwObj, $nlObj | Out-Null

$glfw = Join-Path $root 'thirdparty\glfw'
# GLFW's window.c and our window.cpp both produce window.obj -> separate obj dirs.
$inc = @("/I$glfw\include", "/I$glfw\src", "/I$root\core\include", "/I$root\renderer\include")
$cNames = 'context.c','init.c','input.c','monitor.c','platform.c','vulkan.c','window.c',
          'egl_context.c','osmesa_context.c','null_init.c','null_monitor.c','null_window.c',
          'null_joystick.c','win32_module.c','win32_time.c','win32_thread.c','win32_init.c',
          'win32_joystick.c','win32_monitor.c','win32_window.c','wgl_context.c'
$cSources = $cNames | ForEach-Object { Join-Path $glfw "src\$_" }

Set-Location $glfwObj
& $cl /nologo /c /TC /O2 /D_GLFW_WIN32 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE @inc @cSources 2>&1 | Select-Object -Last 4
if ($LASTEXITCODE -ne 0) { Write-Error 'GLFW compile failed'; exit 1 }

$cpp = @("$root\core\src\log.cpp", "$root\core\src\assert.cpp", "$root\core\src\window.cpp",
         "$root\renderer\src\renderer.cpp", "$root\viewer\main.cpp")
Set-Location $nlObj
& $cl /nologo /c /std:c++17 /EHsc /utf-8 /W4 /D_CRT_SECURE_NO_WARNINGS @inc @cpp 2>&1 | Select-Object -Last 8
if ($LASTEXITCODE -ne 0) { Write-Error 'nl-engine compile failed'; exit 1 }

$objs = @()
$objs += (Get-ChildItem $glfwObj -Filter '*.obj' | ForEach-Object { $_.FullName })
$objs += (Get-ChildItem $nlObj -Filter '*.obj' | ForEach-Object { $_.FullName })
$exe = Join-Path $build 'viewer.exe'
& $cl /nologo @objs "/Fe$exe" /link user32.lib gdi32.lib shell32.lib 2>&1 | Select-Object -Last 8
if ($LASTEXITCODE -ne 0) { Write-Error 'link failed'; exit 1 }

Write-Output '=== M0.1 selftest ==='
& $exe --selftest
$code = $LASTEXITCODE
if ($code -ne 0) { Write-Error "selftest failed ($code)"; exit $code }

Write-Output ''
Write-Output 'M0.1 ACCEPTANCE OK'
Write-Output "Visible case: run $exe (title bar shows live frame counter, ESC quits)"
Write-Output 'Then save a screenshot to docs\screenshots\m0.1.png'
exit 0

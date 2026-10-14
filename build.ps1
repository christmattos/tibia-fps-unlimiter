$ErrorActionPreference = 'Stop'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$src = "$PSScriptRoot\src"
$out = "$PSScriptRoot\bin"
New-Item -ItemType Directory -Force $out | Out-Null
$cmd = "`"$vcvars`" >nul 2>nul && rc /nologo /fo `"$out\app.res`" `"$src\app.rc`" && " +
       "cl /nologo /std:c++20 /utf-8 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE " +
       "/Fo`"$out\\`" /Fe`"$out\TibiaFpsUnlimiter.exe`" `"$src\main.cpp`" `"$src\unlimit.cpp`" `"$out\app.res`" " +
       "/link /SUBSYSTEM:WINDOWS /MANIFEST:NO user32.lib shell32.lib comctl32.lib comdlg32.lib uxtheme.lib advapi32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Remove-Item "$out\*.obj", "$out\app.res"

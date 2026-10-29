param([string]$OutDir=(Join-Path $PSScriptRoot 'bin'), [ValidateSet('x64','x86')][string]$Architecture='x64')
$ErrorActionPreference='Stop'
$vswhere='C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if(!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with C++ desktop tools and Windows SDK first.' }
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs) { throw 'Visual C++ compiler not found.' }
$vc=(Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdkRoot='C:\Program Files (x86)\Windows Kits\10'
$sdkVersion=(Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$toolBin=Join-Path $vc ('bin\Hostx64\'+$Architecture)
$env:PATH=$toolBin+';'+$env:PATH
$env:INCLUDE=(Join-Path $vc 'include')+';'+(Join-Path $sdkRoot "Include\$sdkVersion\ucrt")+';'+(Join-Path $sdkRoot "Include\$sdkVersion\shared")+';'+(Join-Path $sdkRoot "Include\$sdkVersion\um")
$env:LIB=(Join-Path $vc "lib\$Architecture")+';'+(Join-Path $sdkRoot "Lib\$sdkVersion\ucrt\$Architecture")+';'+(Join-Path $sdkRoot "Lib\$sdkVersion\um\$Architecture")
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$OutDir=(Resolve-Path -LiteralPath $OutDir).Path
Push-Location $PSScriptRoot
try {
    & (Join-Path $sdkRoot "bin\$sdkVersion\x64\rc.exe") /nologo /fo (Join-Path $OutDir 'app.res') app.rc
    if($LASTEXITCODE) { throw 'Resource compilation failed.' }
    & (Join-Path $toolBin 'cl.exe') /nologo /utf-8 /std:c++17 /W4 /O1 /MT /EHsc /D_WIN32_WINNT=0x0A00 /DNDEBUG main.cpp (Join-Path $OutDir 'app.res') ('/Fo'+(Join-Path $OutDir 'main.obj')) ('/Fe'+(Join-Path $OutDir 'WindowSizeLock.exe')) /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT user32.lib gdi32.lib comctl32.lib shell32.lib dwmapi.lib
    if($LASTEXITCODE) { throw 'C++ compilation failed.' }
} finally { Pop-Location }
Get-Item -LiteralPath (Join-Path $OutDir 'WindowSizeLock.exe') | Select-Object FullName,Length

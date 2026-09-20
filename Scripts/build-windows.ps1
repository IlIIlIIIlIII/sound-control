[CmdletBinding()]
param([string]$BuildDirectory = 'build\windows-msvc')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& cmake -S $root -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed. Install VS 2022 Desktop development with C++ and a Windows 11 SDK (22000+).' }
& cmake --build $BuildDirectory --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& ctest --test-dir $BuildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
& cmake --install $BuildDirectory --config Release --prefix (Join-Path $BuildDirectory 'package')
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
Write-Output 'Built and tested. The package is unsigned until signed explicitly; this does not install audio effects.'

[CmdletBinding()]
param([string]$BuildDirectory = 'build\windows-msvc',
      [string]$Generator = 'Visual Studio 17 2022',
      [string]$DotNet = 'dotnet')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$configure = @('-S', $root, '-B', $BuildDirectory, '-G', $Generator)
if ($Generator -like 'Visual Studio*') { $configure += @('-A','x64') }
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed. Install VS 2022 Desktop development with C++ and a Windows 11 SDK (22000+).' }
& cmake --build $BuildDirectory --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& ctest --test-dir $BuildDirectory -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
& cmake --install $BuildDirectory --config Release --prefix (Join-Path $BuildDirectory 'package')
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
& $DotNet run --project (Join-Path $root 'Tests\AntigravityBridge\AntigravityBridgeTests.csproj') -c Release
if ($LASTEXITCODE -ne 0) { throw 'Antigravity bridge tests failed.' }
& $DotNet publish (Join-Path $root 'Sources\Windows\WinUI\PersonalToolsWindows.csproj') -c Release -r win-x64 --self-contained true -p:Platform=x64 -o (Join-Path $BuildDirectory 'package\ui')
if ($LASTEXITCODE -ne 0) { throw 'WinUI 3 build failed. Install the .NET 8 SDK.' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'Tests\WindowsInstallerTests.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Installer tests failed.' }
Write-Output 'Built and tested. The package is unsigned until signed explicitly; this does not install audio effects.'

# Optional developer build using already downloaded portable tools; no system installation.
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build\windows-native',
    [string]$PackageDirectory = 'build\winui-package',
    [string]$ToolchainDirectory = 'build\tooling\llvm-mingw-20260908-ucrt-x86_64',
    [string]$SdkDirectory = 'build\tooling\windows-sdk\c\Include\10.0.26100.0\um',
    [string]$CMakeDirectory = 'build\tooling\cmake-4.4.3-windows-x86_64\bin',
    [string]$DotNet = "$env:LOCALAPPDATA\SoundControl\Tooling\dotnet\dotnet.exe"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$toolchain = (Resolve-Path $ToolchainDirectory).Path
$sdk = (Resolve-Path $SdkDirectory).Path
$cmakeBin = (Resolve-Path $CMakeDirectory).Path
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
$headers = Join-Path $BuildDirectory 'sdk-compat'
New-Item -ItemType Directory -Force $headers | Out-Null
# Windows SDK APO interfaces are not shipped by MinGW. Copy just those headers,
# retaining their official GUIDs, and add MinGW's UUID lookup declarations.
foreach ($name in @('audioenginebaseapo.h','audioengineextensionapo.h','audiomediatype.h','AudioAPOTypes.h','mmdeviceapi.h')) {
    $content = Get-Content -LiteralPath (Join-Path $sdk $name) -Raw
    $declarations = [Collections.Generic.List[string]]::new()
    foreach ($match in [regex]::Matches($content, 'MIDL_INTERFACE\("([0-9a-fA-F-]+)"\)\s+(\w+)\s*:')) {
        $parts = $match.Groups[1].Value -split '-'
        $bytes = $parts[3] + $parts[4]
        $values = @("0x$($parts[0])", "0x$($parts[1])", "0x$($parts[2])")
        for ($i = 0; $i -lt 16; $i += 2) { $values += ('0x' + $bytes.Substring($i, 2)) }
        $declarations.Add("__CRT_UUID_DECL($($match.Groups[2].Value),$($values -join ','))")
    }
    if ($name -eq 'mmdeviceapi.h') {
        $declarations.Add('__CRT_UUID_DECL(MMDeviceEnumerator,0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E)')
    }
    $guard = 'SOUNDCONTROL_UUID_' + [IO.Path]::GetFileNameWithoutExtension($name)
    $lines = @($content, "#if defined(__cplusplus) && !defined($guard)", "#define $guard")
    $lines += $declarations
    $lines += '#endif'
    Set-Content -LiteralPath (Join-Path $headers $name) -Encoding utf8 -Value $lines
}
$originalPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $toolchain 'bin');$cmakeBin;$env:PATH"
    $configure = @('-S', $root, '-B', $BuildDirectory, '-G', 'MinGW Makefiles', '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_C_COMPILER=$(Join-Path $toolchain 'bin\x86_64-w64-mingw32-clang.exe')",
        "-DCMAKE_CXX_COMPILER=$(Join-Path $toolchain 'bin\x86_64-w64-mingw32-clang++.exe')",
        ('-DCMAKE_CXX_FLAGS=-I"{0}"' -f $headers.Replace('\','/')))
    & cmake @configure
    if ($LASTEXITCODE -ne 0) { throw 'Native configure failed.' }
    & cmake --build $BuildDirectory --parallel 6
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    & ctest --test-dir $BuildDirectory --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
    & cmake --install $BuildDirectory --prefix $PackageDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Native packaging failed. Close SoundControl before rebuilding.' }
    # The fixture subprocess needs to find the same portable framework as dotnet run.
    $previousDotNetRoot = $env:DOTNET_ROOT
    try {
        $env:DOTNET_ROOT = Split-Path -Parent (Get-Command $DotNet).Source
        & $DotNet run --project (Join-Path $root 'Tests\AntigravityBridge\AntigravityBridgeTests.csproj') -c Release
        if ($LASTEXITCODE -ne 0) { throw 'Antigravity bridge tests failed.' }
    } finally { $env:DOTNET_ROOT = $previousDotNetRoot }
    & $DotNet publish (Join-Path $root 'Sources\Windows\WinUI\SoundControlWindows.csproj') -c Release -r win-x64 --self-contained true -p:Platform=x64 -o (Join-Path $PackageDirectory 'ui')
    if ($LASTEXITCODE -ne 0) { throw 'WinUI build failed.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'Tests\WindowsInstallerTests.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Installer tests failed.' }
    Write-Output "Unsigned package: $PackageDirectory"
} finally { $env:PATH = $originalPath }

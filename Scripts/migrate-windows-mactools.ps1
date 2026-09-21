[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WindowsInstall.psm1') -Force
Assert-Administrator
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$old = Join-Path $env:ProgramFiles 'MacTools'
$target = Join-Path $env:ProgramFiles 'SoundControl'
$data = Join-Path $env:ProgramData 'SoundControl'
$oldData = Join-Path $env:ProgramData 'MacTools'
$backup = Import-Clixml (Join-Path $old 'installation-backup.clixml')
if ($backup.Version -ne 1) { throw 'Unsupported installation backup.' }
if (Test-Path (Join-Path $target 'installation-backup.clixml')) { throw 'SoundControl is already installed.' }
$changes = @()
foreach ($suffix in '01','02') {
    $id = '{4538BFC1-CCED-4C3D-A981-895A737112' + $suffix + '}'
    $path = "HKLM:\SOFTWARE\Classes\CLSID\$id\InprocServer32"
    if ((Get-Item $path).GetValue('') -ne (Join-Path $old 'MacToolsAPO.dll')) { throw "Unexpected APO owner: $id" }
    $changes += Save-RegistryValue $path ''
}
foreach ($name in 'SoundControlAPO.dll','SoundControlSetup.exe','ui\SoundControlBridge.dll','ui\SoundControlWindows.exe','ui\SoundControlWindows.dll','install-windows.ps1','uninstall-windows.ps1','WindowsInstall.psm1') {
    if (!(Test-Path (Join-Path $source $name))) { throw "Missing payload: $name" }
}
$state = [IO.File]::ReadAllBytes((Join-Path $oldData 'state-v1.bin'))
if ($state.Length -ne 17792 -or [BitConverter]::ToInt32($state,0) -ne 0x4d535701 -or ([BitConverter]::ToInt32($state,4) -band 1)) { throw 'Unsupported or busy legacy settings.' }
New-Item -ItemType Directory -Path $target -Force | Out-Null
$recovery = Join-Path $target ('migration-backup-' + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Path $recovery | Out-Null
$changes | Export-Clixml (Join-Path $recovery 'registry.clixml')
Copy-Item -LiteralPath (Join-Path $old 'installation-backup.clixml') -Destination $recovery
Copy-Item -LiteralPath $oldData -Destination (Join-Path $recovery 'MacTools-data') -Recurse
if (Test-Path $data) { Copy-Item -LiteralPath $data -Destination (Join-Path $recovery 'SoundControl-data') -Recurse }
foreach ($name in 'SoundControlAPO.dll','SoundControlSetup.exe','ui','install-windows.ps1','uninstall-windows.ps1','WindowsInstall.psm1') {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $target -Recurse -Force
}
if (Test-Path (Join-Path $old 'npu')) { Copy-Item -LiteralPath (Join-Path $old 'npu') -Destination $target -Recurse -Force }
New-Item -ItemType Directory -Path $data -Force | Out-Null
Get-ChildItem -LiteralPath $oldData -File | Copy-Item -Destination $data -Force
# The v1 settings ABI is unchanged; discard only the old runtime meters.
[Array]::Clear($state, $state.Length - 256, 256)
[IO.File]::WriteAllBytes((Join-Path $data 'state-v1.bin'), $state)
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
& icacls.exe $data /grant "*$($sid):(OI)(CI)M" '*S-1-5-19:(OI)(CI)M' '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Unable to set settings access.' }
try {
    # Keep endpoint modes and protected-audio policy exactly as installed.
    # Carry forward the original pre-install backup for eventual uninstall.
    Copy-Item -LiteralPath (Join-Path $old 'installation-backup.clixml') -Destination (Join-Path $target 'installation-backup.clixml')
    foreach ($entry in $changes) { Set-RegistryValue $entry.Path '' 'String' (Join-Path $target 'SoundControlAPO.dll') }
} catch {
    Restore-RegistryValues $changes
    if (Test-Path (Join-Path $target 'installation-backup.clixml')) { Remove-Item -LiteralPath (Join-Path $target 'installation-backup.clixml') }
    throw
}
Write-Output "Migration registered. Recovery backup: $recovery"

# Exercise registry rollback only in a temporary HKCU key. No elevation or audio changes.
param([string]$ModulePath = (Join-Path $PSScriptRoot '..\Scripts\WindowsInstall.psm1'))
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module $ModulePath -Force
$root = 'HKCU:\Software\SoundControlInstallerTest-' + [guid]::NewGuid().ToString('N')
try {
    Set-RegistryValue $root 'Keep' 'String' 'original'
    Set-RegistryValue $root 'Modes' 'MultiString' ([string[]]@('default','communications'))
    Set-RegistryValue $root 'Enabled' 'DWord' 1
    $backup = @(Save-RegistryValue $root 'Keep'; Save-RegistryValue $root 'Modes'; Save-RegistryValue $root 'Enabled'; Save-RegistryValue "$root\New" '')
    $serialized = [Management.Automation.PSSerializer]::Serialize($backup)
    Set-RegistryValue $root 'Keep' 'String' 'changed'
    Set-RegistryValue $root 'Modes' 'MultiString' ([string[]]@('changed'))
    Set-RegistryValue $root 'Enabled' 'DWord' 0
    Set-RegistryValue "$root\New" '' 'String' 'created'
    Restore-RegistryValues ([Management.Automation.PSSerializer]::Deserialize($serialized))
    $p = Get-ItemProperty $root
    if ($p.Keep -ne 'original' -or $p.Enabled -ne 1 -or ($p.Modes -join ',') -ne 'default,communications' -or (Test-Path "$root\New")) { throw 'Rollback did not preserve original values/types.' }
    Restore-RegistryValues $backup
    Write-Output 'Installer rollback tests passed (isolated HKCU registry key).'
} finally {
    if (Test-Path $root) { Remove-Item $root -Recurse -Force }
}

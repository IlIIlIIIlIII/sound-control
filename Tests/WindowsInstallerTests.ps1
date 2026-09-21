# Exercise registry rollback only in a temporary HKCU key. No elevation or audio changes.
param([string]$ModulePath = (Join-Path $PSScriptRoot '..\Scripts\WindowsInstall.psm1'))
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module $ModulePath -Force
$root = 'HKCU:\Software\SoundControlInstallerTest-' + [guid]::NewGuid().ToString('N')
try {
    if ((Test-PayloadSignature 'NotSigned') -or -not (Test-PayloadSignature 'Valid') -or
        -not (Test-PayloadSignature 'NotSigned' -LocalUnsigned) -or
        (Test-PayloadSignature 'HashMismatch' -LocalUnsigned) -or
        (Test-PayloadSignature 'NotTrusted' -LocalUnsigned)) { throw 'Signature policy regression.' }
    # Cover absent, disabled, and already-enabled protection overrides.
    foreach ($previous in @($null, 0, 1)) {
        $audioKey = "$root\Audio"
        New-Item -Path $audioKey -Force | Out-Null
        Remove-ItemProperty -Path $audioKey -Name 'DisableProtectedAudioDG' -ErrorAction SilentlyContinue
        if ($null -ne $previous) { Set-RegistryValue $audioKey 'DisableProtectedAudioDG' 'DWord' $previous }
        $audioBackup = @(Save-RegistryValue $audioKey 'DisableProtectedAudioDG')
        $audioBackup = [Management.Automation.PSSerializer]::Deserialize([Management.Automation.PSSerializer]::Serialize($audioBackup))
        Set-RegistryValue $audioKey 'DisableProtectedAudioDG' 'DWord' 1
        Restore-RegistryValues $audioBackup
        Restore-RegistryValues $audioBackup
        $audioRegistry = Get-Item $audioKey
        try {
            if ($null -eq $previous) {
                if ($audioRegistry.GetValueNames() -contains 'DisableProtectedAudioDG') { throw 'Originally absent protection override was not removed.' }
            } elseif ($audioRegistry.GetValue('DisableProtectedAudioDG') -ne $previous -or $audioRegistry.GetValueKind('DisableProtectedAudioDG') -ne 'DWord') {
                throw 'Original protection override was not restored.'
            }
        } finally { $audioRegistry.Close() }
    }
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
    # Match MMDevices: value writes allowed, creating subkeys is not allowed.
    $limitedPath = "$root\Limited"
    New-Item -Path $limitedPath -Force | Out-Null
    $originalAcl = Get-Acl $limitedPath
    $limitedHandle = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(($limitedPath -split ':\\',2)[1], $true)
    $limitedAcl = [Security.AccessControl.RegistrySecurity]::new()
    $limitedAcl.SetAccessRuleProtection($true, $false)
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $limitedAcl.AddAccessRule([Security.AccessControl.RegistryAccessRule]::new($sid, [Security.AccessControl.RegistryRights]'ReadKey,SetValue', [Security.AccessControl.AccessControlType]::Allow))
    try {
        Set-Acl -Path $limitedPath -AclObject $limitedAcl
        $limitedBackup = @(Save-RegistryValue $limitedPath 'Effect')
        Set-RegistryValue $limitedPath 'Effect' 'String' 'test-effect'
        if ((Get-ItemProperty $limitedPath).Effect -ne 'test-effect') { throw 'Limited-rights write failed.' }
        Restore-RegistryValues $limitedBackup
        $limitedKey = Get-Item $limitedPath
        if ($limitedKey.GetValueNames() -contains 'Effect') { throw 'Limited-rights rollback failed.' }
        $limitedKey.Close()
    } finally {
        $restoreAcl = [Security.AccessControl.RegistrySecurity]::new()
        $restoreAcl.SetSecurityDescriptorSddlForm($originalAcl.Sddl, [Security.AccessControl.AccessControlSections]::Access)
        $limitedHandle.SetAccessControl($restoreAcl)
        $limitedHandle.Close()
    }
    Write-Output 'Installer rollback tests passed (isolated HKCU registry key).'
} finally {
    if (Test-Path $root) { Remove-Item $root -Recurse -Force }
}

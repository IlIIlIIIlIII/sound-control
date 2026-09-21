Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Test-PayloadSignature([string]$Status, [switch]$LocalUnsigned) {
    # Local mode accepts deliberately unsigned builds, never a broken signature.
    return $Status -eq 'Valid' -or ($LocalUnsigned -and $Status -eq 'NotSigned')
}
function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this installer in an elevated 64-bit PowerShell window.'
    }
    if (-not [Environment]::Is64BitProcess) { throw '64-bit PowerShell is required.' }
}
function Get-EndpointPath([string]$Id, [string]$Flow) {
    $prefix = if ($Flow -eq 'Render') { '{0.0.0.00000000}.' } else { '{0.0.1.00000000}.' }
    if (-not $Id.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Invalid $Flow endpoint ID." }
    $guid = [guid]::Parse($Id.Substring($prefix.Length)).ToString('B')
    $path = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\$Flow\$guid"
    if (-not (Test-Path -LiteralPath $path)) { throw "Endpoint is not present: $Id" }
    if ((Get-ItemProperty -LiteralPath $path).DeviceState -ne 1) { throw "Endpoint is not active: $Id" }
    return "$path\FxProperties"
}
function Save-RegistryValue([string]$Path, [string]$Name) {
    $key = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    $exists = $null -ne $key -and $key.GetValueNames() -contains $Name
    [pscustomobject]@{
        Path = $Path; Name = $Name; KeyExisted = ($null -ne $key); Existed = $exists
        Kind = $(if ($exists) { $key.GetValueKind($Name).ToString() } else { 'String' })
        Value = $(if ($exists) { $key.GetValue($Name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) } else { $null })
    }
}
function Set-RegistryValue([string]$Path, [string]$Name, [string]$Kind, $Value) {
    $parts = $Path -split ':\\', 2
    if ($parts.Count -ne 2) { throw 'Expected an HKLM or HKCU registry path.' }
    $hive = switch ($parts[0]) { 'HKLM' { [Microsoft.Win32.Registry]::LocalMachine } 'HKCU' { [Microsoft.Win32.Registry]::CurrentUser } default { throw 'Unsupported registry hive.' } }
    # Existing endpoint keys allow SetValue but may deny CreateSubKey/WriteKey.
    # Request only the permission needed, preserving the device's original ACL.
    $key = $hive.OpenSubKey($parts[1], [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [Security.AccessControl.RegistryRights]::SetValue)
    if ($null -eq $key) { $key = $hive.CreateSubKey($parts[1], $true) }
    try { $key.SetValue($Name, $Value, [Microsoft.Win32.RegistryValueKind]::$Kind) } finally { $key.Close() }
}
function Restore-RegistryValues($Entries) {
    # Reverse order so empty keys created by this transaction can be removed.
    $reverse = @($Entries); [array]::Reverse($reverse)
    $failures = [Collections.Generic.List[string]]::new()
    foreach ($entry in $reverse) {
      try {
        if ($entry.Existed) {
            $value = $entry.Value
            if ($entry.Kind -eq 'MultiString') { $value = [string[]]$value }
            if ($entry.Kind -eq 'DWord') { $value = [int]$value }
            Set-RegistryValue $entry.Path $entry.Name $entry.Kind $value
        } elseif (Test-Path -LiteralPath $entry.Path) {
            $parts = $entry.Path -split ':\\', 2
            $hive = if ($parts[0] -eq 'HKLM') { [Microsoft.Win32.Registry]::LocalMachine } else { [Microsoft.Win32.Registry]::CurrentUser }
            $existing = Get-Item -LiteralPath $entry.Path
            $hasValue = $existing.GetValueNames() -contains $entry.Name
            $existing.Close()
            if (-not $hasValue -and $entry.KeyExisted) { continue }
            $key = $hive.OpenSubKey($parts[1], [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [Security.AccessControl.RegistryRights]::SetValue)
            try { $key.DeleteValue($entry.Name, $false) } finally { $key.Close() }
            $key = Get-Item -LiteralPath $entry.Path
            $empty = $key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0
            $key.Close()
            if (-not $entry.KeyExisted -and $empty) { Remove-Item -LiteralPath $entry.Path }
        }
      } catch { $failures.Add("$($entry.Path) / $($entry.Name): $_") }
    }
    if ($failures.Count -gt 0) { throw ($failures -join "`n") }
}
Export-ModuleMember -Function Test-PayloadSignature,Assert-Administrator,Get-EndpointPath,Save-RegistryValue,Set-RegistryValue,Restore-RegistryValues

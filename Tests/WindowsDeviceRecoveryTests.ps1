# Pure identity fixtures and isolated HKCU effect/rollback tests; no real devices changed.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '..\Scripts\WindowsDeviceRecovery.psm1') -Force
Import-Module (Join-Path $PSScriptRoot '..\Scripts\WindowsInstall.psm1') -Force
function Device($id,$usb='USB\VID_152A&PID_85DD',$serial='',$container='container-a',$interface='audio-function') {
    [pscustomobject]@{EndpointId=$id;Usb=$usb;Serial=$serial;Container=$container;Interface=$interface}
}
function Check($value,[string]$message) { if (-not $value) { throw $message } }
$old=Device 'old'
Check ((Select-RenderIdentity $old @($old,(Device 'new'))).State -eq 'unchanged') 'Keep active original endpoint'
$new=Device 'new' -container 'container-b'
Check ((Select-RenderIdentity $old @($new)).Method -eq 'unique-usb-model') 'No-serial USB re-enumeration'
Check ((Select-RenderIdentity $old @($new,(Device 'new2' -container 'container-c'))).State -eq 'ambiguous') 'Never choose between identical candidates'
Check ((Select-RenderIdentity $old @()).State -eq 'missing') 'Unplugged endpoint'
Check ((Select-RenderIdentity $old @((Device 'new' -usb 'USB\VID_1234&PID_0001'))).State -eq 'missing') 'Unrelated hardware'
Check ((Select-RenderIdentity $old @((Device 'new' -interface 'another-function'))).State -eq 'missing') 'Different audio function'
$serial=Device 'old' -serial 'ABC'
Check ((Select-RenderIdentity $serial @((Device 'new' -serial 'ABC' -container 'changed'))).Method -eq 'usb-serial') 'Serial follows port changes'
Check ((Select-RenderIdentity $serial @((Device 'new' -serial 'XYZ'))).State -eq 'missing') 'Never downgrade mismatching serial to hardware match'
Check ((Select-RenderIdentity $serial @($new)).State -eq 'missing') 'Missing candidate serial is not a match'
Check ((Select-RenderIdentity $old @((Device 'new'))).Method -eq 'container') 'Container preferred when preserved'

# Mock PnP ancestry verifies serial capability and stops before hub identity.
function global:Get-PnpDeviceProperty {
    param($InstanceId)
    $p=@{}
    if ($InstanceId -like 'SWD*') { $p=@{'DEVPKEY_Device_Parent'='AUDIO\function';'DEVPKEY_Device_ContainerId'='endpoint-container'} }
    elseif ($InstanceId -eq 'AUDIO\function') { $p=@{'DEVPKEY_Device_Parent'='USB\VID_152A&PID_85DD\location';'DEVPKEY_Device_HardwareIds'=@('AUDIO\model')} }
    elseif ($InstanceId -like 'USB\VID_152A*') { $p=@{'DEVPKEY_Device_Parent'='USB\hub';'DEVPKEY_Device_Capabilities'=$global:recoveryTestCapabilities} }
    else { throw 'Must not inspect the hub' }
    foreach($name in $p.Keys) { [pscustomobject]@{KeyName=$name;Data=$p[$name]} }
}
try {
    $global:recoveryTestCapabilities=132
    $identity=Get-RenderIdentity '{0.0.0.00000000}.{00000000-0000-0000-0000-000000000001}'
    Check ($identity.Serial -eq '' -and $identity.Usb -eq 'USB\VID_152A&PID_85DD') 'Location suffix is not a serial'
    $global:recoveryTestCapabilities=148
    $identity=Get-RenderIdentity '{0.0.0.00000000}.{00000000-0000-0000-0000-000000000001}'
    Check ($identity.Serial -eq 'LOCATION') 'Unique capability enables serial identity'
} finally { Remove-Item Function:\Get-PnpDeviceProperty; Remove-Variable recoveryTestCapabilities -Scope Global }

$root='HKCU:\Software\SoundControlRecoveryTest-'+[guid]::NewGuid().ToString('N')
$temp=Join-Path ([IO.Path]::GetTempPath()) ('SoundControlRecovery-'+[guid]::NewGuid().ToString('N')+'.clixml')
try {
    New-Item -Path $root -Force | Out-Null
    $changes=@(Get-RecoveryEqChanges $root)
    $original=@($changes | ForEach-Object { Save-RegistryValue $_.Path $_.Name })
    $backup=[pscustomobject]@{Entries=$original;RenderIdentity=$old;RecoveryPending=$null}
    Save-RecoveryBackup $backup $temp
    foreach($c in $changes) { Set-RegistryValue $c.Path $c.Name $c.Kind $c.Value }
    Check (@(Get-RecoveryEqChanges $root).Count -eq 3) 'Existing own EQ is idempotent'
    $backup.RecoveryPending=[pscustomobject]@{Previous='old';Target='new'}
    Save-RecoveryBackup $backup $temp
    $loaded=Import-Clixml -LiteralPath $temp
    Check ($loaded.RecoveryPending.Target -eq 'new' -and $loaded.Entries.Count -eq 3) 'Durable pending repair and original values'
    Restore-RegistryValues $loaded.Entries
    Check ((Get-Item $root).ValueCount -eq 0) 'Uninstall restores all new endpoint values'
    Set-RegistryValue $root '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1' String '{11111111-1111-1111-1111-111111111111}'
    $rejected=$false
    try { Get-RecoveryEqChanges $root | Out-Null } catch { $rejected=$true }
    Check $rejected 'Preserve OEM effects'
} finally {
    if(Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    if(Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp }
}
Write-Output 'Device identity, ambiguity, serial, atomic metadata and EQ rollback tests passed.'

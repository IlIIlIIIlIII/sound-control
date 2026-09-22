# Switch an existing SoundControl MFX binding to the explicit legacy path.
# Keep the original uninstall backup and all EQ/settings files intact.
[CmdletBinding()]
param([switch]$CheckOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WindowsInstall.psm1') -Force
$directory = Join-Path $env:ProgramFiles 'SoundControl'
$backupPath = Join-Path $directory 'installation-backup.clixml'
$backup = Import-Clixml -LiteralPath $backupPath
if ($backup.Version -ne 1) { throw 'Unsupported installation backup.' }
$capture = Get-EndpointPath $backup.CaptureId 'Capture'
$fx = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}'
$mode = '{d3993a3f-99c2-4402-b5ec-a92a0367664b},6'
$aec = '{4538BFC1-CCED-4C3D-A981-895A73711202}'
$key = Get-Item -LiteralPath $capture
try {
    foreach ($slot in 1,2,5,6,7,13,14,15,19,20) {
        $value = $key.GetValue("$fx,$slot")
        if ($value -and -not ($slot -in 1,6 -and "$value" -eq $aec)) {
            throw "Capture has another effect in slot $slot; refusing to replace it."
        }
    }
    if ($key.GetValue("$fx,1") -eq $aec -and -not $key.GetValue("$fx,6")) {
        Write-Output 'Legacy capture is already registered.'; return
    }
    if ($key.GetValue("$fx,6") -ne $aec) { throw 'Expected the installed SoundControl MFX binding.' }
} finally { $key.Close() }
if ($CheckOnly) {
    Write-Output "Would move SoundControl capture $($backup.CaptureId) from MFX to LFX and restart Windows Audio."
    return
}
Assert-Administrator
$lock = [IO.File]::Open((Join-Path $directory 'device-recovery.lock'),'OpenOrCreate','ReadWrite','None')
try {
    $before = @("$fx,1", "$fx,6", $mode | ForEach-Object { Save-RegistryValue $capture $_ })
    # Back up newly touched values before modifying the endpoint. Existing entries
    # describe the original pre-install state and must never be overwritten.
    foreach ($entry in $before) {
        if (@($backup.Entries | Where-Object { $_.Path -eq $entry.Path -and $_.Name -eq $entry.Name }).Count -eq 0) {
            $backup.Entries = @($backup.Entries) + @($entry)
        }
    }
    $temporary = Join-Path $directory 'capture-backup.pending.clixml'
    $backup | Export-Clixml -LiteralPath $temporary
    [IO.File]::Replace($temporary, $backupPath, (Join-Path $directory 'capture-backup.previous.clixml'))
    try {
        Set-RegistryValue $capture "$fx,1" 'String' $aec
        # Remove only our MFX registration and its mode declaration.
        $relative = ($capture -split ':\\',2)[1]
        $writable = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($relative,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [Security.AccessControl.RegistryRights]::SetValue)
        try { $writable.DeleteValue("$fx,6", $false); $writable.DeleteValue($mode, $false) }
        finally { $writable.Close() }
        Restart-Service Audiosrv -Force
    } catch {
        Restore-RegistryValues $before
        throw
    }
    [IO.File]::WriteAllText((Join-Path $directory 'capture-repair-status.txt'),
        'Legacy capture registered. Reopen recording streams and verify APO callbacks; acoustic reduction is not yet verified.')
    Write-Output 'Legacy capture registered; Windows Audio restarted. Runtime verification required.'
} finally { $lock.Dispose() }

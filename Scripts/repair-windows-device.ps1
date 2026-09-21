# Fixed, protected scheduled-task action. No user-supplied command/path/endpoint.
[CmdletBinding()]
param([switch]$CheckOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WindowsInstall.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'WindowsDeviceRecovery.psm1') -Force
$directory = Join-Path $env:ProgramFiles 'SoundControl'
$backupPath = Join-Path $directory 'installation-backup.clixml'
$statusPath = Join-Path $directory 'device-recovery-status.txt'
if (-not $CheckOnly) { Assert-Administrator }
if (-not (Test-Path -LiteralPath $backupPath)) { return }
function Report([string]$Message) {
    Write-Output $Message
    if (-not $CheckOnly) { [IO.File]::WriteAllText($statusPath, $Message, [Text.UTF8Encoding]::new($false)) }
}
try {
    $lock = $null
    if (-not $CheckOnly) { $lock = [IO.File]::Open((Join-Path $directory 'device-recovery.lock'),'OpenOrCreate','ReadWrite','None') }
    $backup = Import-Clixml -LiteralPath $backupPath
    if ($backup.Version -ne 1) { throw 'Unsupported installation backup.' }
    if (-not $backup.PSObject.Properties['RenderIdentity']) { throw 'Register device recovery first.' }
    $identity = $backup.RenderIdentity
    $identity.EndpointId = $backup.RenderId
    $pending = $backup.PSObject.Properties['RecoveryPending'] -and $null -ne $backup.RecoveryPending
    # Usually no work is needed; avoid traversing all PnP ancestors each minute.
    $endpoint = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\' + ([guid]::Parse(($backup.RenderId -split '\.')[4])).ToString('B')
    $current = Get-ItemProperty -LiteralPath $endpoint -ErrorAction SilentlyContinue
    if ($current -and $current.DeviceState -eq 1 -and -not $pending) {
        if ((Test-Path -LiteralPath $statusPath) -and (Get-Content -LiteralPath $statusPath -Raw) -notlike 'Recovered*') { Report 'Selected speaker connected; automatic device recovery is enabled.' }
        return
    }
    $selection = Select-RenderIdentity $identity @(Get-ActiveRenderIdentities)
    if ($selection.State -eq 'missing') { Report 'Selected speaker is disconnected; waiting for the same hardware.'; return }
    if ($selection.State -eq 'ambiguous') { Report 'Multiple matching speakers found. Select the intended device; automatic recovery is paused.'; return }
    if ($selection.State -eq 'unchanged' -and -not $pending) { return }
    $target = $selection.Device.EndpointId
    if ($pending -and $backup.RecoveryPending.Target -ne $target) { throw 'Pending repair targets another endpoint; manual recovery required.' }
    $path = Get-EndpointPath $target 'Render'
    $changes = @(Get-RecoveryEqChanges $path)
    if ($CheckOnly) { Report ("Would recover {0} -> {1} using {2}; EQ registration and audio restart required." -f $backup.RenderId,$target,$selection.Method); return }
    if (-not $pending) {
        # Save original values BEFORE any mutation. Keep every endpoint for uninstall.
        foreach ($change in $changes) {
            if (@($backup.Entries | Where-Object { $_.Path -eq $change.Path -and $_.Name -eq $change.Name }).Count -eq 0) {
                $backup.Entries = @($backup.Entries) + @(Save-RegistryValue $change.Path $change.Name)
            }
        }
        $backup | Add-Member -NotePropertyName RecoveryPending -NotePropertyValue ([pscustomobject]@{Previous=$backup.RenderId;Target=$target}) -Force
        Save-RecoveryBackup $backup $backupPath
    }
    foreach ($change in $changes) { Set-RegistryValue $change.Path $change.Name $change.Kind $change.Value }
    $process = Start-Process -FilePath (Join-Path $directory 'SoundControlSetup.exe') -ArgumentList @('--rebind-render', $backup.RecoveryPending.Previous, $target, $backup.CaptureId) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Settings rebind failed ($($process.ExitCode)); recovery retained for retry/uninstall." }
    # Rebuild the graph after changing the endpoint and reference mapping.
    Restart-Service Audiosrv -Force
    $backup.RenderId = $target
    $backup.RenderIdentity = $selection.Device
    $backup.RecoveryPending = $null
    Save-RecoveryBackup $backup $backupPath
    Report ("Recovered speaker via {0} at {1}; EQ registered and audio restarted." -f $selection.Method,(Get-Date -Format o))
} catch {
    Report ("Device recovery needs attention: $_")
    throw
} finally {
    if ($lock) { $lock.Dispose() }
}

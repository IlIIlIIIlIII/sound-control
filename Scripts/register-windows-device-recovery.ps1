[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WindowsInstall.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'WindowsDeviceRecovery.psm1') -Force
Assert-Administrator
$directory = Join-Path $env:ProgramFiles 'PersonalTools'
$backupPath = Join-Path $directory 'installation-backup.clixml'
$backup = Import-Clixml -LiteralPath $backupPath
if ($backup.Version -ne 1) { throw 'Unsupported installation backup.' }
if (-not $backup.PSObject.Properties['RenderIdentity']) {
    $identity = Get-RenderIdentity $backup.RenderId
    $backup | Add-Member -NotePropertyName RenderIdentity -NotePropertyValue $identity
    Save-RecoveryBackup $backup $backupPath
}
$script = Join-Path $directory 'repair-windows-device.ps1'
if (-not (Test-Path -LiteralPath $script)) { throw 'Recovery payload is not installed.' }
# The elevated action and its identity/rollback metadata live in Program Files,
# never in the writable settings folder. Task has no arbitrary command inputs.
$action = New-ScheduledTaskAction -Execute "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $script + '"')
$triggers = @((New-ScheduledTaskTrigger -AtStartup), (New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 1)))
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
Register-ScheduledTask -TaskName 'PersonalTools Device Recovery' -Action $action -Trigger $triggers -Settings $settings -User 'SYSTEM' -RunLevel Highest -Force | Out-Null
Write-Output 'Device recovery registered: startup and once per minute; audio restarts only when an endpoint changes.'

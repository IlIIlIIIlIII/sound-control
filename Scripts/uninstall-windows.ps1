[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WindowsInstall.psm1') -Force
Assert-Administrator
$directory = Join-Path $env:ProgramFiles 'PersonalTools'
$backupPath = Join-Path $directory 'installation-backup.clixml'
if (-not (Test-Path -LiteralPath $backupPath)) { throw 'No installation backup found. No audio settings changed.' }
# Prevent a concurrent repair from re-applying effects during rollback.
$task = Get-ScheduledTask -TaskName 'PersonalTools Device Recovery' -ErrorAction SilentlyContinue
if ($task) {
    Disable-ScheduledTask -InputObject $task | Out-Null
    Stop-ScheduledTask -InputObject $task
    Unregister-ScheduledTask -InputObject $task -Confirm:$false
}
$lock = [IO.File]::Open((Join-Path $directory 'device-recovery.lock'),'OpenOrCreate','ReadWrite','None')
try {
$backup = Import-Clixml -LiteralPath $backupPath
if ($backup.Version -ne 1) { throw 'Unsupported backup version.' }
# Keep recovery data if any restoration fails. Re-running is idempotent.
Restore-RegistryValues $backup.Entries
Remove-Item -LiteralPath $backupPath
} finally { $lock.Dispose() }
Write-Output 'Original effect registrations and backed-up Protected Audio setting restored. Restart Windows Audio or reboot to unload existing audio graphs. Imported EQ/settings and binaries are preserved; no virtual device was created.'

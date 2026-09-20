[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WindowsInstall.psm1') -Force
Assert-Administrator
$directory = Join-Path $env:ProgramFiles 'SoundControl'
$backupPath = Join-Path $directory 'installation-backup.clixml'
if (-not (Test-Path -LiteralPath $backupPath)) { throw 'No installation backup found. No audio settings changed.' }
$backup = Import-Clixml -LiteralPath $backupPath
if ($backup.Version -ne 1) { throw 'Unsupported backup version.' }
# Keep recovery data if any restoration fails. Re-running is idempotent.
Restore-RegistryValues $backup.Entries
Remove-Item -LiteralPath $backupPath
Write-Output 'Original effect registrations restored. Reconnect SMSL and MOTU (or reboot) to unload existing audio graphs. Imported EQ/settings and binaries are preserved; no virtual device was created.'

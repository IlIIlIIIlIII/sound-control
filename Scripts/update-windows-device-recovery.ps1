# Upgrade an existing SoundControl installation without changing APO/model binaries.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WindowsInstall.psm1') -Force
Assert-Administrator
$destination = Join-Path $env:ProgramFiles 'SoundControl'
$backupPath = Join-Path $destination 'installation-backup.clixml'
$installation = Import-Clixml -LiteralPath $backupPath
if ($installation.Version -ne 1) { throw 'Unsupported installation.' }
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$files = @('SoundControlSetup.exe','WindowsInstall.psm1','WindowsDeviceRecovery.psm1','install-windows.ps1','uninstall-windows.ps1','repair-windows-device.ps1','register-windows-device-recovery.ps1',
    'ui\SoundControlBridge.dll','ui\SoundControlWindows.exe','ui\SoundControlWindows.dll','ui\SoundControlWindows.deps.json','ui\SoundControlWindows.runtimeconfig.json','ui\SoundControlWindows.pri')
foreach ($file in $files) {
    $path = Join-Path $source $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing payload: $file" }
    if ([IO.Path]::GetExtension($file) -in @('.dll','.exe')) {
        $signature = Get-AuthenticodeSignature -LiteralPath $path
        if (-not (Test-PayloadSignature $signature.Status.ToString() -LocalUnsigned:([bool]$installation.LocalUnsigned))) { throw "Invalid payload signature: $file" }
    }
}
$recovery = Join-Path $destination ('device-recovery-backup-' + (Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Path (Join-Path $recovery 'ui') -Force | Out-Null
Copy-Item -LiteralPath $backupPath -Destination $recovery
$task = Get-ScheduledTask -TaskName 'SoundControl Device Recovery' -ErrorAction SilentlyContinue
if ($task) { Disable-ScheduledTask -InputObject $task | Out-Null; Stop-ScheduledTask -InputObject $task }
foreach ($process in @(Get-Process SoundControlWindows -ErrorAction SilentlyContinue)) {
    if ($process.Path -eq (Join-Path $destination 'ui\SoundControlWindows.exe')) { Stop-Process -Id $process.Id; $process.WaitForExit() }
}
$copied = @()
try {
    foreach ($file in $files) {
        $target = Join-Path $destination $file
        if (Test-Path -LiteralPath $target) { Copy-Item -LiteralPath $target -Destination (Join-Path $recovery $file) }
        Copy-Item -LiteralPath (Join-Path $source $file) -Destination $target -Force
        $copied += $file
    }
} catch {
    foreach ($file in $copied) {
        $original = Join-Path $recovery $file
        if (Test-Path -LiteralPath $original) { Copy-Item -LiteralPath $original -Destination (Join-Path $destination $file) -Force }
    }
    if ($task) { Enable-ScheduledTask -InputObject $task | Out-Null }
    throw
}
# After installation, retain the new repair tools and journal if repair fails.
# Reverting executables would make a pending transaction impossible to resume.
& (Join-Path $destination 'register-windows-device-recovery.ps1')
& (Join-Path $destination 'repair-windows-device.ps1')
Write-Output "Device recovery installed. Previous application files: $recovery"

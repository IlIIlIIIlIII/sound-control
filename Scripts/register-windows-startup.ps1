[CmdletBinding()]
param([string]$Executable = "$env:ProgramFiles\SoundControl\ui\SoundControlWindows.exe")
$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute $Executable -Argument '--startup' -WorkingDirectory (Split-Path $Executable)
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
# Give Explorer time to initialize the notification area after sign-in.
$trigger.Delay = 'PT15S'
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName 'SoundControl' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description 'Start SoundControl in the system tray after sign-in.' -Force | Out-Null
# Remove the old entry only after the replacement task has been registered.
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name SoundControl -ErrorAction SilentlyContinue
Write-Output "SoundControl will start in the notification area at sign-in: $Executable"

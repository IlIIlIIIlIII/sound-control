# Local endpoint installation. Does not disable Protected Audio, Secure Boot,
# driver signature checks, or replace the vendor's USB drivers.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RenderId,
    [Parameter(Mandatory)][string]$CaptureId,
    [string]$SourceDirectory = '',
    [switch]$CheckOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($SourceDirectory)) { $SourceDirectory = $PSScriptRoot }
Import-Module (Join-Path $PSScriptRoot 'WindowsInstall.psm1') -Force
$render = Get-EndpointPath $RenderId 'Render'
$capture = Get-EndpointPath $CaptureId 'Capture'
$build = [int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber
if ($build -lt 22000) { throw 'Windows 11 build 22000 or later is required for CAPX AEC.' }
$eq = '{4538BFC1-CCED-4C3D-A981-895A73711201}'
$aec = '{4538BFC1-CCED-4C3D-A981-895A73711202}'
$fx = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}'
$modes = '{d3993a3f-99c2-4402-b5ec-a92a0367664b}'
$defaultMode = '{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}'
$communicationsMode = '{98951333-B9CD-48B1-A0A3-FF40682D73F7}'
$destination = Join-Path $env:ProgramFiles 'MacTools'
$backupPath = Join-Path $destination 'installation-backup.clixml'
if (Test-Path -LiteralPath $backupPath) { throw 'Already installed or recovery pending. Run uninstall-windows.ps1 before changing endpoints or updating.' }
foreach ($endpoint in @($render,$capture)) {
    $key = Get-Item -LiteralPath $endpoint -ErrorAction SilentlyContinue
    if ($key) {
        foreach ($name in $key.GetValueNames()) {
            # Do not silently replace/chainsaw an OEM effect chain.
            if ($name -match '^\{d04e05a6-594b-4fb6-a80d-01af5eed7d1d\},(1|2|5|6|7|13|14|15|19|20)$') {
                $value = $key.GetValue($name)
                if ($value -and "$value" -ne '{00000000-0000-0000-0000-000000000000}') {
                    throw "Existing effects on $endpoint ($name). This local installer only binds endpoints with no existing APO chain."
                }
            }
        }
        $key.Close()
    }
}
$payload = @('MacToolsAPO.dll','MacToolsWindows.exe','install-windows.ps1','uninstall-windows.ps1','WindowsInstall.psm1')
foreach ($name in $payload) {
    if (-not (Test-Path -LiteralPath (Join-Path $SourceDirectory $name))) { throw "Missing payload: $name. Use cmake --install output." }
}
foreach ($name in @('MacToolsAPO.dll','MacToolsWindows.exe')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $SourceDirectory $name)
    if ($signature.Status -ne 'Valid') {
        throw "Installation blocked: $name signature is $($signature.Status). Supply signed binaries. No audio settings have been changed. A trusted Authenticode signature alone does not guarantee Protected Audio acceptance; actual loading must also be verified."
    }
}
if ($CheckOnly) {
    Write-Output 'Preflight passed. No files or audio settings were changed. Protected Audio loading still requires runtime verification.'
    return
}
Assert-Administrator
# Build a complete reversible list before touching endpoint/COM values.
$changes = [Collections.Generic.List[object]]::new()
function Add-Change($path,$name,$kind,$value) { $changes.Add([pscustomobject]@{Path=$path;Name=$name;Kind=$kind;Value=$value}) }
foreach ($item in @(@($eq,'MacTools stereo EQ',15),@($aec,'MacTools microphone echo cancellation',12))) {
    $id=$item[0]; $title=$item[1]; $flags=$item[2]
    $classPath="HKLM:\SOFTWARE\Classes\CLSID\$id"
    $apoPath="HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$id"
    if ((Test-Path $classPath) -or (Test-Path $apoPath)) { throw "Registration already exists without an installation backup: $id" }
    Add-Change $classPath '' 'String' $title
    Add-Change "$classPath\InprocServer32" '' 'String' (Join-Path $destination 'MacToolsAPO.dll')
    Add-Change "$classPath\InprocServer32" 'ThreadingModel' 'String' 'Both'
    Add-Change $apoPath 'FriendlyName' 'String' $title
    Add-Change $apoPath 'Copyright' 'String' 'MacTools'
    foreach ($pair in @(@('MajorVersion',1),@('MinorVersion',0),@('Flags',$flags),@('MinInputConnections',1),@('MaxInputConnections',1),@('MinOutputConnections',1),@('MaxOutputConnections',1),@('MaxInstances',-1),@('NumAPOInterfaces',1))) {
        Add-Change $apoPath $pair[0] 'DWord' ([int]$pair[1])
    }
    Add-Change $apoPath 'APOInterface0' 'String' '{FD7F2B29-24D0-4B5C-B177-592C39F9CA10}'
}
Add-Change $render "$fx,7" 'String' $eq
Add-Change $render "$modes,7" 'MultiString' ([string[]]@($defaultMode))
Add-Change $capture "$fx,6" 'String' $aec
Add-Change $capture "$modes,6" 'MultiString' ([string[]]@($defaultMode,$communicationsMode))
# Preserve and enable the endpoint system-effects switch.
Add-Change $render '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5' 'DWord' 0
Add-Change $capture '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5' 'DWord' 0
$original = @($changes | ForEach-Object { Save-RegistryValue $_.Path $_.Name })
New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($name in $payload) {
    $source = [IO.Path]::GetFullPath((Join-Path $SourceDirectory $name))
    $target = [IO.Path]::GetFullPath((Join-Path $destination $name))
    if ($source -ne $target) { Copy-Item -LiteralPath $source -Destination $target -Force }
}
# Recovery metadata is admin-owned in Program Files, never in user-writable data.
[pscustomobject]@{Version=1;RenderId=$RenderId;CaptureId=$CaptureId;Entries=$original} | Export-Clixml -LiteralPath $backupPath
try {
    $data = Join-Path $env:ProgramData 'MacTools'
    New-Item -ItemType Directory -Path $data -Force | Out-Null
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    & icacls.exe $data /grant "*$($sid):(OI)(CI)M" '*S-1-5-19:(OI)(CI)M' '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Unable to set audio-engine settings access.' }
    $p = Start-Process -FilePath (Join-Path $destination 'MacToolsWindows.exe') -ArgumentList @('--initialize',"`"$RenderId`"","`"$CaptureId`"") -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "Initial device/EQ configuration failed ($($p.ExitCode)). Check E:\Speaker\L.txt and R.txt." }
    foreach ($change in $changes) { Set-RegistryValue $change.Path $change.Name $change.Kind $change.Value }
} catch {
    $failure = $_
    try { Restore-RegistryValues $original; Remove-Item -LiteralPath $backupPath }
    catch { throw "Installation failed ($failure); rollback also failed ($_). Keep $backupPath and run uninstall-windows.ps1 elevated." }
    throw "Installation failed; original effect registrations restored. $failure"
}
Write-Output 'Registered; actual processing is NOT yet verified. Reconnect SMSL and MOTU (or reboot), reopen shared-mode playback/capture apps, then inspect the MacTools status window. Protected Audio rejection requires a correctly signed APO package; this installer does not weaken system protections.'
Start-Process -FilePath (Join-Path $destination 'MacToolsWindows.exe')

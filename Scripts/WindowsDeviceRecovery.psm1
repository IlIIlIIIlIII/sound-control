Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RenderIdentity([string]$EndpointId) {
    if ($EndpointId -notmatch '^\{0\.0\.0\.00000000\}\.\{[0-9a-f-]{36}\}$') { throw 'Invalid render endpoint.' }
    $node = 'SWD\MMDEVAPI\' + $EndpointId
    $container = ''; $interface = ''; $usb = ''; $serial = ''
    # Stop at the FIRST physical USB device. A hub's serial/container is not
    # the DAC's identity. Phantom nodes retain information after unplugging.
    for ($depth = 0; $depth -lt 10 -and $node; $depth++) {
        $properties = @(Get-PnpDeviceProperty -InstanceId $node -ErrorAction Stop)
        $values = @{}
        foreach ($p in $properties) { $values[$p.KeyName] = $p.Data }
        if ($depth -eq 0) { $container = [string]$values['DEVPKEY_Device_ContainerId'] }
        if ($depth -eq 1) {
            $interface = (@($values['DEVPKEY_Device_HardwareIds']) | Sort-Object) -join '|'
        }
        if ($node -match '^(USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4})\\(.+)$') {
            $usb = $Matches[1].ToUpperInvariant()
            # CM_DEVCAP_UNIQUEID: location-generated instance suffixes are NOT serials.
            if (([uint32]$values['DEVPKEY_Device_Capabilities'] -band 0x10) -ne 0) {
                $serial = $Matches[2].ToUpperInvariant()
            }
            break
        }
        $node = [string]$values['DEVPKEY_Device_Parent']
    }
    [pscustomobject]@{ EndpointId=$EndpointId; Usb=$usb; Serial=$serial; Container=$container; Interface=$interface }
}

function Select-RenderIdentity($Identity, [object[]]$Candidates) {
    $exact = @($Candidates | Where-Object { $_.EndpointId -eq $Identity.EndpointId })
    if ($exact.Count -eq 1) { return [pscustomobject]@{State='unchanged'; Device=$exact[0]; Method='endpoint'} }
    # Match audio function too: a USB device may expose multiple outputs.
    $compatible = @($Candidates | Where-Object { $Identity.Interface -and $_.Interface -eq $Identity.Interface })
    $matches = @(); $method = ''
    if ($Identity.Serial -and $Identity.Usb) {
        $matches = @($compatible | Where-Object { $_.Usb -eq $Identity.Usb -and $_.Serial -eq $Identity.Serial })
        $method = 'usb-serial'
        # Never fall back to a different serial, even if the model matches.
    } else {
        if ($Identity.Container -and $Identity.Container -notin @('{00000000-0000-0000-0000-000000000000}','{00000000-0000-0000-ffff-ffffffffffff}')) {
            $matches = @($compatible | Where-Object { $_.Container -eq $Identity.Container -and $_.Usb -eq $Identity.Usb })
            $method = 'container'
        }
        if ($matches.Count -eq 0 -and $Identity.Usb) {
            $matches = @($compatible | Where-Object { $_.Usb -eq $Identity.Usb -and -not $_.Serial })
            $method = 'unique-usb-model'
        }
    }
    if ($matches.Count -gt 1) { return [pscustomobject]@{State='ambiguous';Device=$null;Method=$method} }
    if ($matches.Count -eq 1) { return [pscustomobject]@{State='rebind';Device=$matches[0];Method=$method} }
    return [pscustomobject]@{State='missing';Device=$null;Method=$method}
}

function Get-ActiveRenderIdentities {
    $root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
    foreach ($key in Get-ChildItem -LiteralPath $root) {
        if ((Get-ItemProperty -LiteralPath $key.PSPath).DeviceState -eq 1) {
            # An unreadable candidate must abort the scan: ignoring it could
            # turn two identical devices into a falsely unique candidate.
            Get-RenderIdentity ('{0.0.0.00000000}.' + $key.PSChildName)
        }
    }
}

function Save-RecoveryBackup($Backup, [string]$Path) {
    $temporary = $Path + '.new'
    $Backup | Export-Clixml -LiteralPath $temporary
    if (Test-Path -LiteralPath $Path) { [IO.File]::Replace($temporary,$Path,[NullString]::Value) }
    else { [IO.File]::Move($temporary,$Path) }
}

function Get-RecoveryEqChanges([string]$Path) {
    $fx = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}'
    $eq = '{4538BFC1-CCED-4C3D-A981-895A73711201}'
    $key = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if ($key) {
        try {
            foreach ($name in $key.GetValueNames()) {
                if ($name -match '^\{d04e05a6-594b-4fb6-a80d-01af5eed7d1d\},(1|2|5|6|7|13|14|15|19|20)$') {
                    $value = $key.GetValue($name)
                    if ($value -and "$value" -ne '{00000000-0000-0000-0000-000000000000}' -and
                        -not ($name -eq "$fx,7" -and "$value" -eq $eq)) { throw 'The replacement endpoint already has another audio effect. Select/configure it manually.' }
                }
            }
        } finally { $key.Close() }
    }
    @(
        [pscustomobject]@{Path=$Path;Name="$fx,7";Kind='String';Value=$eq}
        [pscustomobject]@{Path=$Path;Name='{d3993a3f-99c2-4402-b5ec-a92a0367664b},7';Kind='MultiString';Value=[string[]]@('{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}')}
        [pscustomobject]@{Path=$Path;Name='{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5';Kind='DWord';Value=0}
    )
}
Export-ModuleMember -Function Get-RenderIdentity,Select-RenderIdentity,Get-ActiveRenderIdentities,Save-RecoveryBackup,Get-RecoveryEqChanges

param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$RuntimeLicenseDirectory,
    [Parameter(Mandatory)][string]$ModelDirectory,
    [Parameter(Mandatory)][string]$ModelLicense,
    [Parameter(Mandatory)][string]$PackageDirectory
)
$ErrorActionPreference = 'Stop'
$hashes = @{
    'dtln_aec_256_1.tflite'='4A3A588B69FD79D837BC068B579A26FAA92CAC39DDDBB00001D2DC1C3D869D60'
    'dtln_aec_256_2.tflite'='FA2590243AAD1BF893C5BE45B20709E8C50FEEC65E3604D1D52BAE6EEDDC23D3'
}
foreach($name in $hashes.Keys){
    if((Get-FileHash -LiteralPath (Join-Path $ModelDirectory $name) -Algorithm SHA256).Hash -ne $hashes[$name]){
        throw "Unexpected DTLN-AEC model hash: $name"
    }
}
foreach($name in @('openvino_c.dll','openvino.dll','openvino_intel_npu_plugin.dll','openvino_tensorflow_lite_frontend.dll')){
    if(!(Test-Path -LiteralPath (Join-Path $RuntimeDirectory $name))){throw "Missing runtime: $name"}
}
if(!(Test-Path -LiteralPath $ModelLicense)){throw 'Missing model licence'}
if(!(Test-Path -LiteralPath (Join-Path $RuntimeLicenseDirectory 'LICENSE'))){throw 'Missing OpenVINO licence'}
$target=Join-Path $PackageDirectory 'npu'
New-Item -ItemType Directory -Force -Path $target | Out-Null
Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' | Copy-Item -Destination $target -Force
foreach($name in $hashes.Keys){Copy-Item -LiteralPath (Join-Path $ModelDirectory $name) -Destination $target -Force}
Copy-Item -LiteralPath $ModelLicense -Destination (Join-Path $target 'LICENSE-DTLN-AEC.txt') -Force
Copy-Item -LiteralPath (Join-Path $RuntimeLicenseDirectory 'LICENSE') -Destination (Join-Path $target 'LICENSE-OpenVINO.txt') -Force
Get-ChildItem -LiteralPath (Join-Path $RuntimeLicenseDirectory 'licensing') -File | Copy-Item -Destination $target -Force
Get-ChildItem -LiteralPath $target -File | Get-FileHash -Algorithm SHA256 |
    Select-Object @{Name='file';Expression={Split-Path $_.Path -Leaf}},Hash |
    ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $target 'manifest.json')
'DTLN-AEC 256; NPU only; 16 kHz; 4096 frames latency at 48 kHz' |
    Set-Content -Encoding utf8 (Join-Path $target 'enabled.flag')

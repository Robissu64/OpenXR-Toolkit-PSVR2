$ErrorActionPreference = 'Stop'

$RegistryPath = 'HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
$JsonPath = Join-Path $PSScriptRoot 'XR_APILAYER_MBUCCHIA_toolkit.json'

if (Test-Path -Path $RegistryPath) {
    if ((Get-Item -Path $RegistryPath).Property -contains $JsonPath) {
        Remove-ItemProperty -Path $RegistryPath -Name $JsonPath
    }

    if ((Get-Item -Path $RegistryPath).Property -contains $JsonPath) {
        throw 'OpenXR layer registration could not be removed.'
    }
}

$ErrorActionPreference = 'Stop'

$RegistryPath = 'HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
$JsonPath = Join-Path $PSScriptRoot 'XR_APILAYER_MBUCCHIA_toolkit.json'

if (-not (Test-Path -LiteralPath $JsonPath)) {
    throw "OpenXR layer manifest not found: $JsonPath. Extract the entire ZIP before installing."
}

New-Item -Path $RegistryPath -Force | Out-Null

# Keep Ultraleap after this layer in the registry ordering, preserving its enabled state.
$ultraleapPath = (Get-Item -Path $RegistryPath).Property |
    Where-Object { $_ -match '\\UltraleapHandTracking\.json$' } |
    Select-Object -First 1

if ($ultraleapPath) {
    $ultraleapValue = Get-ItemPropertyValue -Path $RegistryPath -Name $ultraleapPath
    Remove-ItemProperty -Path $RegistryPath -Name $ultraleapPath
    try {
        New-ItemProperty -Path $RegistryPath -Name $JsonPath -PropertyType DWord -Value 0 -Force | Out-Null
    } finally {
        New-ItemProperty -Path $RegistryPath -Name $ultraleapPath -PropertyType DWord -Value $ultraleapValue -Force | Out-Null
    }
} else {
    New-ItemProperty -Path $RegistryPath -Name $JsonPath -PropertyType DWord -Value 0 -Force | Out-Null
}

if ((Get-ItemPropertyValue -Path $RegistryPath -Name $JsonPath) -ne 0) {
    throw 'OpenXR layer registration could not be verified.'
}

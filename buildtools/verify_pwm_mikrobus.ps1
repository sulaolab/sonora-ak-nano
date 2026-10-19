# Verify saved Nano artifacts only. No flash/reset/UART operations.
[CmdletBinding()]
param(
    [ValidateSet(2, 3)][int]$Slot = 3,
    [string]$ImageDir,
    [switch]$ScopeOnly,
    [switch]$HalAudio = $true
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($ImageDir)) {
    $area = if ($ScopeOnly) { 'pwm_scope' } else { 'pwm_mikrobus' }
    $halSuffix = if ($HalAudio) { '-hal-audio' } else { '' }
    $pointer = Join-Path $repoRoot "artifacts/$area/latest-slot$Slot$halSuffix.txt"
    $relative = (Get-Content -LiteralPath $pointer -Raw).Trim()
    $ImageDir = Join-Path $repoRoot $relative
}
$ImageDir = (Resolve-Path -LiteralPath $ImageDir).Path
$ready = Get-Content -LiteralPath (Join-Path $ImageDir 'ready.json') -Raw | ConvertFrom-Json
$savedHal = [bool]$ready.pwm_audio_hal # older manifests are the direct implementation
if ($savedHal -ne [bool]$HalAudio) { throw 'HAL/direct implementation differs from the requested image.' }
if ($savedHal -and ($ready.hal_source_commit -ne 'cc4bdda46a5654567aa92aeaec8cca2c6db5b86c' -or
    $ready.extra_defines -notcontains 'APP_PWM_AUDIO_HAL=1')) { throw 'Common HAL source/settings are missing.' }
if ([bool]$ready.scope_only -ne $ScopeOnly.IsPresent) { throw 'Scope-only/normal mode differs from the requested image.' }
if ($ScopeOnly -and ($ready.mode -ne 'scope-only' -or $ready.pwm_gain_db -ne 0 -or
    $ready.pwm_mapping -ne 'tdm_tx_q31' -or
    [string]::IsNullOrWhiteSpace($ready.scope_tag) -or
    $ready.extra_defines -notcontains 'PWM_AUDIO_SCOPE_ONLY=1' -or
    $ready.extra_defines -notcontains "PWM_AUDIO_SCOPE_TAG=$($ready.scope_tag)")) {
    throw 'Scope-only build identity or settings are missing.'
}
if ($ready.device -ne 'dsPIC33AK512MPS506' -or $ready.monitor_profile -notin @('ak-nano', 'nano') -or
    $ready.configuration -ne 'dsPIC33AK512MPS506_CLASSIC' -or $ready.serial_update_support -or
    $ready.application_profile -ne 'APP_BUILD_STD_DEMO_1' -or $ready.pwm_output -ne 1 -or
    $ready.codec_slot -ne 1 -or $ready.pwm_slot -ne $Slot -or $ready.carrier_hz -notin @(48000, 96000, 192000)) {
    throw 'Saved image does not match the requested Nano first-light setup.'
}
# Old manifests describe the original 48 kHz build; new rates must name their define.
if ($ready.carrier_hz -ne 48000 -and
    ($ready.carrier_factor -ne ($ready.carrier_hz / 48000) -or
     $ready.extra_defines -notcontains "MPQ6612A_CARRIER_FACTOR=$($ready.carrier_factor)")) {
    throw 'Saved carrier setting is missing or inconsistent.'
}
$name = [string]$ready.hex_file
if ([string]::IsNullOrWhiteSpace($name) -or [IO.Path]::GetFileName($name) -ne $name) {
    throw 'HEX filename must be a basename.'
}
$hex = Join-Path $ImageDir $name
if ((Get-FileHash -LiteralPath $hex -Algorithm SHA256).Hash -ne $ready.hex_sha256) {
    throw 'Saved HEX hash differs from ready.json.'
}
$meta = Get-Content -LiteralPath (Join-Path $ImageDir 'factory-build.json') -Raw | ConvertFrom-Json
if ($meta.device -ne $ready.device -or $meta.application_profile -ne $ready.application_profile -or
    $meta.resolved_configuration -ne $ready.configuration -or $meta.serial_update_support) {
    throw 'Factory build metadata disagrees with the saved selection.'
}
Write-Host "Verified Nano slot $Slot ROM image: $hex"
Write-Host "SHA256: $($ready.hex_sha256)"
Write-Host "Source: $($ready.source_commit)"
Write-Host "Carrier: $($ready.carrier_hz) Hz; PWM gain: $($ready.pwm_gain_db) dB"
Write-Host "HAL audio: $savedHal"
Write-Host "Mode: $(if ($ScopeOnly) { 'scope-only (nSLEEP Low)' } else { 'normal MPQ' })"
Write-Host 'When hardware is available, copy this factory HEX to the intended Nano mass-storage programmer.'

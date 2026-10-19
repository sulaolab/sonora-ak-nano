# Prepare a saved Nano PWM first-light ROM image. Does not access hardware.
[CmdletBinding()]
param([ValidateSet(2, 3)][int]$Slot = 3, [switch]$ScopeOnly, [switch]$HalAudio = $true,
    # POT (AVAS pitch) is on by default; -Pot:$false turns it off. Slot 2 PWM
    # shares the POT pin, so slot 2 defaults it off (explicit -Pot is a #error).
    [ValidateSet(48000, 96000, 192000)][int]$CarrierHz = 192000, [switch]$Pot = $true,
    # Prototype PG alignment (pwm_audio_board_ak.c): edge|hren|centre|centre-hren|3level.
    # hren (edge + HREN) is the firmware default; the others are explicit A/B builds.
    # 3level = MPQ6612A brake-zero Dual PWM (theme 3, not adopted), 192 kHz carrier only.
    [ValidateSet('edge', 'hren', 'centre', 'centre-hren', '3level')][string]$Align = 'hren')

$ErrorActionPreference = 'Stop'
if ($Slot -eq 2 -and !$PSBoundParameters.ContainsKey('Pot')) { $Pot = [switch]$false }
if ($Align -eq '3level' -and $CarrierHz -ne 192000) { throw '-Align 3level needs -CarrierHz 192000.' }
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$configuration = 'dsPIC33AK512MPS506_CLASSIC'
$carrierFactor = [int]($CarrierHz / 48000)
$carrierK = [int]($CarrierHz / 1000)
$defines = @("MPQ6612A_CARRIER_FACTOR=$carrierFactor", 'APP_PWM_OUTPUT=1', 'APP_REQ_MIKROB_WM8904=0', "MPQ6612A_NANO_SLOT=$Slot")
$defines += "ENA_NANO_POT=$([int]$Pot.IsPresent)"
$defines += "APP_PWM_AUDIO_HAL=$([int][bool]$HalAudio)"
$scopeTag = 'ak_scope_' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff')
$mode = if ($ScopeOnly) { 'scope-only' } else { 'normal' }
$area = if ($ScopeOnly) { 'pwm_scope' } else { 'pwm_mikrobus' }
$defines += "PWM_AUDIO_SCOPE_ONLY=$([int]$ScopeOnly.IsPresent)"
$defines += "PWM_AUDIO_AK_CENTRE=$([int]($Align -like 'centre*'))"
$defines += "PWM_AUDIO_AK_HREN=$([int]($Align -like '*hren' -or $Align -eq '3level'))"
$defines += "PWM_AUDIO_AK_3LEVEL=$([int]($Align -eq '3level'))"
if ($ScopeOnly) { $defines += "PWM_AUDIO_SCOPE_TAG=$scopeTag" }
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'switch_config.ps1') `
    -SerialUpdateSupport No -Device dsPIC33AK512MPS506 -Profile 'Classic 1' -Root $repoRoot
if ($LASTEXITCODE -ne 0) { throw 'Nano Classic 1 selection failed.' }
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Full -Root $repoRoot `
    -Define ($defines -join ',')
if ($LASTEXITCODE -ne 0) { throw 'Nano PWM build failed.' }

$production = Join-Path $repoRoot "dspic33ak_audio_dsp.X/dist/$configuration/production"
$factory = Join-Path $production 'dspic33ak_audio_dsp.X.factory.production.hex'
$factoryMeta = [IO.Path]::ChangeExtension($factory, '.json')
$meta = Get-Content -LiteralPath $factoryMeta -Raw | ConvertFrom-Json
if ($meta.device -ne 'dsPIC33AK512MPS506' -or $meta.application_profile -ne 'APP_BUILD_STD_DEMO_1' -or
    $meta.serial_update_support -or $meta.resolved_configuration -ne $configuration) {
    throw 'Unexpected Nano factory image selection.'
}
$sourceCommit = & git -C $repoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot record source commit.' }
$sourceStatus = @(& git -C $repoRoot status --porcelain=v1)
if ($LASTEXITCODE -ne 0) { throw 'Cannot record source status.' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$halSuffix = if ($HalAudio) { '-hal-audio' } else { '' }
$output = Join-Path $repoRoot "artifacts/$area/$stamp-slot$Slot$halSuffix-$Align"
New-Item -ItemType Directory -Path $output | Out-Null
$hexName = "nano_ak506_mpq6612a_slot${Slot}_${carrierK}k.factory.hex"
if ($ScopeOnly) { $hexName = "nano_ak506_mpq6612a_slot${Slot}_${carrierK}k_scope-only.factory.hex" }
if ($HalAudio) { $hexName = $hexName.Replace('.factory.hex', '_hal-audio.factory.hex') }
if ($Align -ne 'hren') { $hexName = $hexName.Replace('.factory.hex', "_$Align.factory.hex") }
Copy-Item -LiteralPath $factory -Destination (Join-Path $output $hexName)
Copy-Item -LiteralPath $factoryMeta -Destination (Join-Path $output 'factory-build.json')
# Include new source files in provenance without staging or committing them.
& git -C $repoRoot diff --binary HEAD | Set-Content -LiteralPath (Join-Path $output 'source.patch') -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw 'Cannot record source delta.' }
$newFiles = @(& git -C $repoRoot ls-files --others --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate new source files.' }
foreach ($rel in $newFiles) {
    $destination = Join-Path $output "new-source/$rel"
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot $rel) -Destination $destination
}
$ready = [ordered]@{
    device = 'dsPIC33AK512MPS506'
    application_profile = 'APP_BUILD_STD_DEMO_1'
    configuration = $configuration
    monitor_profile = 'ak-nano'
    serial_update_support = $false
    pwm_output = 1
    pwm_audio_hal = [bool]$HalAudio
    hal_source_commit = if ($HalAudio) { 'cc4bdda46a5654567aa92aeaec8cca2c6db5b86c' } else { $null }
    hal_canonical_commit = if ($HalAudio) { 'f6e06819657f09d3f6cb75f1709e7c1837d25dc7' } else { $null }
    adapter_source_commit = if ($HalAudio) { '80fa855ef775e8e7cdd4d4721a5c75fef7fd4b61' } else { $null }
    mode = $mode
    scope_only = $ScopeOnly.IsPresent
    scope_tag = if ($ScopeOnly) { $scopeTag } else { $null }
    pwm_gain_db = if ($ScopeOnly) { 0 } else { 12 }
    pwm_mapping = if ($ScopeOnly) { 'tdm_tx_q31' } else { 'processed_float' }
    nano_pot_enabled = $Pot.IsPresent
    codec_slot = 1
    pwm_slot = $Slot
    carrier_hz = $CarrierHz
    carrier_factor = $carrierFactor
    pwm_align = $Align
    audio_channel = 'L'
    extra_defines = $defines
    source_commit = [string]$sourceCommit
    source_status = $sourceStatus
    built_at = (Get-Date).ToString('o')
    hex_file = $hexName
    hex_sha256 = (Get-FileHash -LiteralPath (Join-Path $output $hexName) -Algorithm SHA256).Hash
}
$ready | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'ready.json') -Encoding utf8
$relativeOutput = "artifacts/$area/$stamp-slot$Slot$halSuffix-$Align"
$relativeOutput | Set-Content -LiteralPath (Join-Path $repoRoot "artifacts/$area/latest.txt") -Encoding utf8
$relativeOutput | Set-Content -LiteralPath (Join-Path $repoRoot "artifacts/$area/latest-slot$Slot$halSuffix.txt") -Encoding utf8
Write-Host "Nano PWM first-light ROM image ready: $output"
Write-Host "Mode: $mode; HAL: $([bool]$HalAudio); Verify: pwsh buildtools/verify_pwm_mikrobus.ps1 -Slot $Slot -ScopeOnly:`$$($ScopeOnly.IsPresent) -HalAudio:`$$([bool]$HalAudio)"

# Compare the common PWM implementation with the reviewed Sonora checkout.
# Read-only. Pass the checkout explicitly; no local path is stored in the repo.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SonoraRoot)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$SonoraRoot = (Resolve-Path -LiteralPath $SonoraRoot).Path
$commonFiles = @(
    'src/app/apps/classic/classic_audio_pwm.c',
    'src/app/apps/classic/classic_audio_pwm.h',
    'src/app/apps/classic/classic_audio_path.c',
    'src/app/apps/classic/classic_console.c',
    'src/app/hal_pwm/nora_pwm.h',
    'src/app/hal_pwm/nora_pwm_dspic33ak.c',
    'src/app/pwm_audio_dma_buffer.h',
    'src/app/board/audio/mpq6612a_board.h',
    'src/app/board/audio/nora_pwm_audio_conf.h',
    'src/app/board/audio/pwm_audio_board.h'
)
foreach ($rel in $commonFiles) {
    $localText = [IO.File]::ReadAllText((Join-Path $repoRoot $rel)).Replace("`r`n", "`n")
    $peerText = [IO.File]::ReadAllText((Join-Path $SonoraRoot $rel)).Replace("`r`n", "`n")
    if ($localText -cne $peerText) { throw "Common PWM source differs: $rel" }
    Write-Host "PASS $rel"
}
function Get-PwmConversion([string]$Root, [string]$Name = 'convert_float_to_pwm_20bit') {
    $text = [IO.File]::ReadAllText((Join-Path $Root 'src/app/apps/shared/float_conversion.c'))
    $start = $text.IndexOf('void ' + $Name + '(')
    if ($start -lt 0) { throw "Cannot locate PWM conversion in $Root" }
    $body = $text.IndexOf('{', $start)
    $depth = 0
    for ($i = $body; $i -lt $text.Length; $i++) {
        if ($text[$i] -eq '{') { $depth++ }
        if ($text[$i] -eq '}') {
            $depth--
            if ($depth -eq 0) {
                return $text.Substring($start, $i - $start + 1).Replace("`r`n", "`n")
            }
        }
    }
    throw "Unclosed PWM conversion function in $Root"
}
if ((Get-PwmConversion $repoRoot) -cne (Get-PwmConversion $SonoraRoot)) {
    throw 'PWM conversion differs from Sonora.'
}
Write-Host 'PASS convert_float_to_pwm_20bit()'

if ((Get-PwmConversion $repoRoot 'convert_codec_q31_to_pwm_20bit') -cne
    (Get-PwmConversion $SonoraRoot 'convert_codec_q31_to_pwm_20bit')) {
    throw 'TDM Q31 PWM conversion differs from Sonora.'
}
Write-Host 'PASS convert_codec_q31_to_pwm_20bit()'
if ((Get-PwmConversion $repoRoot 'convert_float_to_pwm_q31') -cne
    (Get-PwmConversion $SonoraRoot 'convert_float_to_pwm_q31')) {
    throw 'HAL Q31 input conversion differs from Sonora.'
}
Write-Host 'PASS convert_float_to_pwm_q31()'
$backendRel = 'src/app/board/audio/pwm_audio_board_ak.c'
$backendMarker = 'void pwm_audio_board_scope_print('
$localBackend = [IO.File]::ReadAllText((Join-Path $repoRoot $backendRel)).Replace("`r`n", "`n")
$peerBackend = [IO.File]::ReadAllText((Join-Path $SonoraRoot $backendRel)).Replace("`r`n", "`n")
$localEnd = $localBackend.IndexOf($backendMarker)
$peerEnd = $peerBackend.IndexOf($backendMarker)
if ($localEnd -lt 0 -or $peerEnd -lt 0 -or
    $localBackend.Substring(0, $localEnd) -cne $peerBackend.Substring(0, $peerEnd)) {
    throw 'PWM board algorithms differ from Sonora (only scope diagnostics may differ).'
}
Write-Host 'PASS board backend algorithms (scope diagnostics are board-specific)'
foreach ($halFile in (Get-ChildItem -LiteralPath (Join-Path $SonoraRoot 'src/app/hal_pwm_audio') -File)) {
    $localBytes = [IO.File]::ReadAllBytes((Join-Path $repoRoot "src/app/hal_pwm_audio/$($halFile.Name)"))
    $peerBytes = [IO.File]::ReadAllBytes($halFile.FullName)
    if ([Convert]::ToBase64String($localBytes) -cne [Convert]::ToBase64String($peerBytes)) {
        throw "Common HAL differs byte-for-byte: $($halFile.Name)"
    }
    Write-Host "PASS exact common HAL $($halFile.Name)"
}

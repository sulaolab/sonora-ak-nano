# Target-specific Nano scope-only programming through verified mass storage.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$KitSerial,
    [Parameter(Mandatory)][string]$Drive,
    [Parameter(Mandatory)][string]$MonitorUrl,
    [Parameter(Mandatory)][string]$MonitorPort,
    [ValidateSet('ak-nano', 'nano', 'default')][string]$MonitorProfile = 'ak-nano',
    [Parameter(Mandatory)][string]$ImageDir
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ImageDir = (Resolve-Path -LiteralPath $ImageDir).Path
$ready = Get-Content -LiteralPath (Join-Path $ImageDir 'ready.json') -Raw | ConvertFrom-Json
& (Join-Path $PSScriptRoot 'verify_pwm_mikrobus.ps1') -Slot $ready.pwm_slot -ScopeOnly -ImageDir $ImageDir -HalAudio:([bool]$ready.pwm_audio_hal)
$driveRoot = (Resolve-Path -LiteralPath $Drive).Path
$kit = Get-Content -LiteralPath (Join-Path $driveRoot 'KIT-INFO.TXT') -Raw
if ($kit -notmatch ('(?m)^Kit USB serial number:\s*' + [regex]::Escape($KitSerial) + '\s*$') -or
    $kit -notmatch '(?m)^Device:\s*dsPIC33AK512MPS506\s*$' -or
    $kit -notmatch '(?m)^Drag and drop:\s*Yes\s*$') { throw 'Nano kit/device/drag-and-drop identity mismatch.' }
$uri = [uri]$MonitorUrl
if (!$uri.IsLoopback -or $uri.Scheme -ne 'http') { throw 'Use the resolved loopback monitor URL.' }
$base = $MonitorUrl.TrimEnd('/')
$status = Invoke-RestMethod "$base/status" -TimeoutSec 5
if (!$status.connected -or $status.profile -ne $MonitorProfile -or $status.port -ne $MonitorPort -or $status.tx_gate.held_by) {
    throw 'Monitor target is disconnected, differs, or holds a transfer gate.'
}
# Bind the named UART to this USB kit, rather than trusting a healthy HTTP reply.
$serialDevice = Get-CimInstance Win32_SerialPort | Where-Object DeviceID -eq $MonitorPort
if (!$serialDevice) { throw 'Cannot identify the target UART device.' }
$cursor = $serialDevice.PNPDeviceID
$sameKit = $false
for ($depth = 0; $depth -lt 8 -and $cursor; ++$depth) {
    if ($cursor.EndsWith('\' + $KitSerial, [StringComparison]::OrdinalIgnoreCase)) { $sameKit = $true; break }
    $cursor = (Get-PnpDeviceProperty -InstanceId $cursor -KeyName 'DEVPKEY_Device_Parent' -ErrorAction Stop).Data
}
if (!$sameKit) { throw 'UART does not belong to the selected kit serial.' }
$proofDir = Join-Path $ImageDir ('flash-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $proofDir | Out-Null
$kit | Set-Content -LiteralPath (Join-Path $proofDir 'kit-info.txt')
$status | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $proofDir 'monitor-before.json')
$mute = Invoke-RestMethod "$base/command_wait" -Method Post -ContentType application/json `
    -Body (@{cmd='*ts'; contains='analog mute verified, TDM/DMA halted'; timeout=10} | ConvertTo-Json) -TimeoutSec 15
$mute | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $proofDir 'mute.json')
if (!$mute.matched) { throw 'Analog mute/transport stop was not confirmed for this scope experiment.' }
# Arm before copying; cached STATUS.TXT cannot prove that this image booted.
$waitJob = Start-ThreadJob -ScriptBlock {
    param($Base, $Tag)
    Invoke-RestMethod "$Base/wait" -Method Post -ContentType application/json `
        -Body (@{contains=$Tag; timeout=55} | ConvertTo-Json) -TimeoutSec 60
} -ArgumentList $base, $ready.scope_tag
try {
    Start-Sleep -Milliseconds 600
    $hex = Join-Path $ImageDir $ready.hex_file
    Copy-Item -LiteralPath $hex -Destination (Join-Path $driveRoot $ready.hex_file)
    $boot = Receive-Job -Job $waitJob -Wait -ErrorAction Stop
    $boot | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $proofDir 'boot.json')
    if (!$boot.matched -and !$boot.ok) { throw 'The new scope build ID was not observed; no flash success claim.' }
    Write-Host "Scope image booted: $($ready.scope_tag); target $KitSerial; evidence $proofDir"
    Write-Host 'Measure slot 3 RST/nSLEEP Low before probing PWM/CS IN1/IN2.'
} finally {
    Remove-Job -Job $waitJob -Force -ErrorAction SilentlyContinue
}

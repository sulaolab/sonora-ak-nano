# Program a saved normal HAL audio ROM through the explicitly identified Nano.
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
$ImageDir = (Resolve-Path -LiteralPath $ImageDir).Path
$ready = Get-Content (Join-Path $ImageDir 'ready.json') -Raw | ConvertFrom-Json
& (Join-Path $PSScriptRoot 'verify_pwm_mikrobus.ps1') -Slot $ready.pwm_slot -ImageDir $ImageDir -HalAudio:([bool]$ready.pwm_audio_hal)
$driveRoot = (Resolve-Path -LiteralPath $Drive).Path
$kit = Get-Content (Join-Path $driveRoot 'KIT-INFO.TXT') -Raw
if ($kit -notmatch ('(?m)^Kit USB serial number:\s*' + [regex]::Escape($KitSerial) + '\s*$') -or
    $kit -notmatch '(?m)^Device:\s*dsPIC33AK512MPS506\s*$' -or
    $kit -notmatch '(?m)^Drag and drop:\s*Yes\s*$') { throw 'Nano kit/device identity mismatch.' }
$uri = [uri]$MonitorUrl
if (!$uri.IsLoopback -or $uri.Scheme -ne 'http') { throw 'Use the resolved loopback monitor URL.' }
$base = $MonitorUrl.TrimEnd('/')
$status = Invoke-RestMethod "$base/status" -TimeoutSec 5
if (!$status.connected -or $status.profile -ne $MonitorProfile -or $status.port -ne $MonitorPort -or $status.tx_gate.held_by) {
    throw 'Monitor target is disconnected, differs, or holds a transfer gate.'
}
$serialDevice = Get-CimInstance Win32_SerialPort | Where-Object DeviceID -eq $MonitorPort
if (!$serialDevice) { throw 'Target UART device missing.' }
$cursor = $serialDevice.PNPDeviceID
$sameKit = $false
for ($depth = 0; $depth -lt 8 -and $cursor; ++$depth) {
    if ($cursor.EndsWith('\' + $KitSerial, [StringComparison]::OrdinalIgnoreCase)) { $sameKit = $true; break }
    $cursor = (Get-PnpDeviceProperty -InstanceId $cursor -KeyName DEVPKEY_Device_Parent -ErrorAction Stop).Data
}
if (!$sameKit) { throw 'UART does not belong to the selected kit.' }
$proofDir = Join-Path $ImageDir ('flash-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$hex = Join-Path $ImageDir $ready.hex_file
$driveInfo = [IO.DriveInfo]::new($driveRoot)
if ($driveInfo.AvailableFreeSpace -lt (Get-Item -LiteralPath $hex).Length) {
    throw 'Nano drive has insufficient space. Preserve/remove the previous HEX from this verified drive before retrying.'
}
New-Item -ItemType Directory -Path $proofDir | Out-Null
$kit | Set-Content (Join-Path $proofDir 'kit-info.txt')
$status | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $proofDir 'monitor-before.json')
# Finish any partial line left by a previous console client.
$null = Invoke-RestMethod "$base/command" -Method Post -ContentType application/json -Body (@{cmd="`n"} | ConvertTo-Json) -TimeoutSec 5
$mute = Invoke-RestMethod "$base/command_wait" -Method Post -ContentType application/json `
    -Body (@{cmd='*ts';contains='analog mute verified, TDM/DMA halted';timeout=10} | ConvertTo-Json) -TimeoutSec 15
$mute | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $proofDir 'mute.json')
if (!$mute.matched) { throw 'Analog mute/transport stop not confirmed.' }
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$short = (& git -C $repoRoot rev-parse --short $ready.source_commit).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve ROM source identity.' }
$marker = 'Commit: ' + $short + $(if (@($ready.source_status).Count) { '_dirty' } else { '' })
$waitJob = Start-ThreadJob -ScriptBlock {
    param($Base,$Marker)
    Invoke-RestMethod "$Base/wait" -Method Post -ContentType application/json `
        -Body (@{contains=$Marker;timeout=55} | ConvertTo-Json) -TimeoutSec 60
} -ArgumentList $base,$marker
try {
    Start-Sleep -Milliseconds 600
    Copy-Item -LiteralPath $hex -Destination (Join-Path $driveRoot $ready.hex_file)
    $boot = Receive-Job -Job $waitJob -Wait -ErrorAction Stop
    $boot | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $proofDir 'boot.json')
    if (!$boot.matched) { throw 'Expected boot commit was not observed.' }
    # The source commit can be shared by several dirty builds. Read the actual
    # PWM period/rate after boot to distinguish the newly requested ROM.
    if ($null -ne $ready.carrier_factor) {
        Start-Sleep -Seconds 2
        $runtime = Invoke-RestMethod "$base/command_wait" -Method Post -ContentType application/json `
            -Body (@{cmd='?cp';contains='$80cp';timeout=10} | ConvertTo-Json) -TimeoutSec 15
        $runtime | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $proofDir 'pwm-runtime.json')
        # command_wait returns only the fragment containing the marker. Fetch
        # the complete reply from buffered traffic before checking its fields.
        $runtimeLog = Invoke-RestMethod "$base/log?tail=100" -TimeoutSec 5
        $runtimeLog | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $proofDir 'pwm-runtime-log.json')
        $runtimeText = $runtimeLog.lines -join "`n"
        # HREN=0 reads PER[3:0] as zero (DS70005591D 15.4.31 note 2).
        # The PWM driver writes period-1, so compare the masked readback.
        $expectedPer = ([int](798720000 / $ready.carrier_hz) * 16 - 1) -band 0xFFFF0
        # Alignment prototypes write PER = span - 16 Q4 (pwm_audio_board_ak.c);
        # centre-aligned halves the span. Same value as above for plain edge.
        $align = if ($ready.pwm_align) { [string]$ready.pwm_align } else { 'edge' }
        if ($align -ne 'edge') {
            $span = [int](798720000 / $ready.carrier_hz) * 16
            if ($align -like 'centre*') { $span /= 2 }
            $expectedPer = $span - 16
            $hren = [int]($align -like '*hren' -or $align -eq '3level')
            $alignOk = $runtimeText -match ("PWMalign centre=$([int]($align -like 'centre*')) hren=$hren\b") -and
                $runtimeText -match ("PWMshape 3level=$([int]($align -eq '3level'))\b") -and
                $runtimeText -match 'hrerr=0 hr_err=0\b'
            if (!$alignOk) { throw "Booted PWM alignment does not match '$align'." }
        }
        if (!$runtime.matched -or $runtimeText -notmatch "carrier=$($ready.carrier_hz)\b" -or
            $runtimeText -notmatch "per=${expectedPer}\b") {
            throw 'Booted PWM carrier/PG5PER does not match the saved ROM settings.'
        }
    }
    Write-Host "Normal audio ROM booted: $marker; kit $KitSerial; SHA256 $($ready.hex_sha256); evidence $proofDir"
    Write-Host 'Lab bench control: *cp5501 start + nSLEEP High; *cp5500 stop + nSLEEP Low; ?cp status.'
} finally {
    Remove-Job -Job $waitJob -Force -ErrorAction SilentlyContinue
}

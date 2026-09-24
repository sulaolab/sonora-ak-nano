# Clean build outputs for one configuration without depending on editor tooling.
#
# This helper removes outputs for the configuration selected by build.ps1.
$ErrorActionPreference = 'Continue'

if ([string]::IsNullOrWhiteSpace($env:MPLABX_CONF)) {
    Write-Error 'MPLABX_CONF is not set. Invoke buildtools\build.ps1 (which supplies the configuration), or set MPLABX_CONF explicitly before calling clean.ps1.'
    exit 2
}
$conf = $env:MPLABX_CONF
$targets = @("build\$conf", "dist\$conf")
$lockedFiles = 0

foreach ($target in $targets) {
    if (-not (Test-Path -LiteralPath $target)) {
        Write-Host "already clean: $target"
        continue
    }

    for ($attempt = 1; $attempt -le 3; $attempt++) {
        Get-ChildItem -LiteralPath $target -Recurse -File -Force -ErrorAction SilentlyContinue |
            ForEach-Object {
                try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop } catch {}
            }
        try { Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop } catch {}
        if (-not (Test-Path -LiteralPath $target)) { break }
        Start-Sleep -Milliseconds 300
    }

    $left = @(Get-ChildItem -LiteralPath $target -Recurse -File -Force -ErrorAction SilentlyContinue)
    $lockedFiles += $left.Count
    if (-not (Test-Path -LiteralPath $target)) {
        Write-Host "cleaned: $target"
    } elseif ($left.Count -eq 0) {
        Write-Host "cleaned (empty dirs still held by mplab_backend remain - harmless): $target"
    } else {
        Write-Host "WARNING: $($left.Count) output file(s) still locked under $target"
    }
}

if ($lockedFiles -gt 0) {
    Write-Error "Clean incomplete: $lockedFiles locked output file(s). Fully exit MPLAB X (including the mplab_backend process) and retry."
    exit 1
}
exit 0

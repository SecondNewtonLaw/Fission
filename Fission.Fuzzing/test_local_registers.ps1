param(
    [Parameter(Mandatory = $true)][string]$Fuzzer,
    [string]$Manifest = (Join-Path $PSScriptRoot 'local_register_regressions.txt'),
    [string]$RepoRoot = (Split-Path $PSScriptRoot -Parent)
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Fuzzer).Path
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$manifestPath = (Resolve-Path -LiteralPath $Manifest).Path
$samples = Get-Content -LiteralPath $manifestPath | Where-Object { $_ -and -not $_.StartsWith('#') }
$failures = @()

foreach ($relativePath in $samples) {
    $sample = Join-Path $root $relativePath
    if (-not (Test-Path -LiteralPath $sample -PathType Leaf)) {
        $failures += "$relativePath`: fixture missing"
        Write-Host "[FAIL] $relativePath (fixture missing)" -ForegroundColor Red
        continue
    }

    $diagnostic = (& $exe --roblox-recompile $sample 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        $failures += "$relativePath`n$diagnostic"
        Write-Host "[FAIL] $relativePath" -ForegroundColor Red
    } else {
        Write-Host "[PASS] $relativePath" -ForegroundColor Green
    }
}

Write-Host "$($samples.Count) local-register fixtures checked; $($failures.Count) failed."
if ($failures.Count -ne 0) {
    throw ($failures -join "`n`n")
}

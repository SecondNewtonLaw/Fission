$RepoRoot = "F:\Coding\cxx_cpp\Fission"
$CliExe = Join-Path $RepoRoot "cmake-build-debug\Fission.CLI.exe"
$TestFile = Join-Path $RepoRoot "test.txt"
$ResultsDir = Join-Path $RepoRoot "Samples\StressTests\results"

New-Item -ItemType Directory -Path $ResultsDir -Force | Out-Null

$TestSamples = Get-ChildItem -Path "F:\Coding\cxx_cpp\Fission\Samples\StressTests" -Filter "*.lua" | Sort-Object Name

foreach ($sample in $TestSamples) {
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($sample.Name)
    Write-Host "=== Running $baseName ===" -ForegroundColor Cyan

    # Copy source to test.txt
    Copy-Item -LiteralPath $sample.FullName -Destination $TestFile -Force

    # Run CLI and capture both stdout and stderr
    $output = & $CliExe 2>&1 | Out-String
    $exitCode = $LASTEXITCODE

    # Save stdout/stderr
    $output | Out-File -FilePath (Join-Path $ResultsDir "$baseName.stdout.txt") -Force

    # Save ir_out.txt if it exists
    $irFile = Join-Path $RepoRoot "ir_out.txt"
    if (Test-Path $irFile) {
        Copy-Item -LiteralPath $irFile -Destination (Join-Path $ResultsDir "$baseName.ir_out.txt") -Force
    }

    # Save cfg.dot if it exists
    $dotFile = Join-Path $RepoRoot "cfg.dot"
    if (Test-Path $dotFile) {
        Copy-Item -LiteralPath $dotFile -Destination (Join-Path $ResultsDir "$baseName.cfg.dot") -Force
    }

    Write-Host "  Exit code: $exitCode" -ForegroundColor Green
    Write-Host "--------------------------------------------------"
}

Write-Host "All tests completed. Results in: $ResultsDir" -ForegroundColor Cyan

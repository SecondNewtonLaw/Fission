param(
    [string]$ResultsDir = (Join-Path $PSScriptRoot "results"),
    [switch]$OptimizeIR
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$CliExe = Join-Path $RepoRoot "cmake-build-debug\Fission.CLI.exe"

New-Item -ItemType Directory -Path $ResultsDir -Force | Out-Null

$ResultsDir = (Resolve-Path -LiteralPath $ResultsDir).Path
$TestSamples = Get-ChildItem -LiteralPath $PSScriptRoot -Filter "*.lua" | Sort-Object Name
$Failures = 0

Push-Location -LiteralPath $ResultsDir
try {
    foreach ($sample in $TestSamples) {
        $baseName = [System.IO.Path]::GetFileNameWithoutExtension($sample.Name)
        Write-Host "=== Running $baseName ===" -ForegroundColor Cyan

        $args = @("--decompile-test", $sample.FullName)
        if ($OptimizeIR) {
            $args += "--optimize-ir"
        }
        $decompileStdout = Join-Path $ResultsDir ".decompile.stdout.tmp"
        $decompileStderr = Join-Path $ResultsDir ".decompile.stderr.tmp"
        & $CliExe @args 1> $decompileStdout 2> $decompileStderr
        $exitCode = $LASTEXITCODE
        $stdout = [System.IO.File]::ReadAllText($decompileStdout)
        $stderr = [System.IO.File]::ReadAllText($decompileStderr)
        Remove-Item -LiteralPath $decompileStdout, $decompileStderr -Force
        $output = $stdout + $stderr
        if ($exitCode -eq 0) {
            $sourceMatch = [regex]::Match($stdout, '(?s)===SOURCE===\r?\n(.*?)\r?\n===END===')
            if ($sourceMatch.Success) {
                $sourceFile = Join-Path $ResultsDir "$baseName.out.lua"
                $sourceMatch.Groups[1].Value | Out-File -LiteralPath $sourceFile -Encoding utf8
                $parseOutput = & $CliExe --parse-check $sourceFile 2>&1 | Out-String
                $exitCode = $LASTEXITCODE
                $output += $parseOutput
            } else {
                $exitCode = 1
            }
        }
        if ($exitCode -ne 0) {
            $Failures++
        }

        $output | Out-File -FilePath (Join-Path $ResultsDir "$baseName.stdout.txt") -Force
        $irFile = Join-Path $ResultsDir "ir_out.txt"
        if ($exitCode -eq 0 -and (Test-Path -LiteralPath $irFile)) {
            Copy-Item -LiteralPath $irFile -Destination (Join-Path $ResultsDir "$baseName.ir_out.txt") -Force
        }

        Write-Host "  Exit code: $exitCode"
    }
} finally {
    Pop-Location
}

Write-Host "$($TestSamples.Count) samples completed; $Failures failed. Results in: $ResultsDir" -ForegroundColor Cyan
if ($Failures -ne 0) {
    exit 1
}

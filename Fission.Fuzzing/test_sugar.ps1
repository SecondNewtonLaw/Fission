param(
    [Parameter(Mandatory = $true)][string]$Fuzzer,
    [string]$ResultsDir = (Join-Path $PSScriptRoot '../_codex_sugar/smoke')
)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Fuzzer).Path
$results = [IO.Path]::GetFullPath($ResultsDir)
New-Item -ItemType Directory -Path $results -Force | Out-Null
$output = & $exe --sugar-only --count 200 --seed 914 --threads 1 --max-corpus 200 --out $results 2>&1
$exitCode = $LASTEXITCODE
$log = $output -join "`n"
Set-Content -LiteralPath (Join-Path $results 'report.log') -Value $log -Encoding utf8
if ($exitCode -ne 0) { throw "Sugar campaign failed with exit $exitCode. See $results/report.log" }
$counts = @{}
foreach ($match in [regex]::Matches($log, '(?m)^\s+(\S+)\s+(\d+)\s*$')) {
    $counts[$match.Groups[1].Value] = [int]$match.Groups[2].Value
}
foreach ($family in @('sugar-compound', 'sugar-methods', 'sugar-interactions')) {
    $generated = $counts["family:${family}:generated"]
    if (!$generated -or $generated -ne $counts["family:${family}:match"]) {
        throw "Family $family missing or not semantically matched"
    }
}
foreach ($form in @('compound-+=', 'compound--=', 'compound-*=', 'compound-/=', 'compound-//=',
        'compound-%=', 'compound-^=', 'compound-..=', 'method-definition', 'method-call',
        'record-field', 'elseif', 'if-expression', 'local-function', 'continue')) {
    if (!$counts["sugar:${form}:input"]) { throw "Sugar form was not generated/measured: $form" }
}
if ($counts['SUGAR_PARSE_ERROR']) { throw 'Sugar AST parsing failed' }
$checks = @(
    @{
        Name = 'cross-function'; Exit = 1; Finding = 'chunk/first|compound-+=|arg0.x'
        Source = 'local function first(t) t.x += 1 end; local function second(t) t.x += 1 end; local t={x=0}; first(t); second(t); print(t.x)'
        Output = 'local function first(t) t.x = t.x + 1 end; local function second(t) t.x += 0; t.x += 1 end; local t={x=0}; first(t); second(t); print(t.x)'
    },
    @{
        Name = 'cross-target'; Exit = 1; Finding = 'chunk/update|compound-+=|arg0.x'
        Source = 'local function update(t) t.x += 1; t.y += 2 end; local t={x=0,y=0}; update(t); print(t.x,t.y)'
        Output = 'local function update(t) t.x = t.x + 1; t.y += 0; t.y += 2 end; local t={x=0,y=0}; update(t); print(t.x,t.y)'
    },
    @{
        Name = 'shadowed-functions'; Exit = 1; Finding = 'chunk/update|compound-+=|arg0.x'
        Source = 'local t={x=0}; local function update(t) t.x += 1 end; do local function update(t) t.x += 1 end; update(t) end; update(t); print(t.x)'
        Output = 'local t={x=0}; local function update(t) t.x = t.x + 1 end; do local function update(t) t.x += 0; t.x += 1 end; update(t) end; update(t); print(t.x)'
    },
    @{
        Name = 'return-inline'; Exit = 0; Finding = ''
        Source = 'local function update(x) x += 1; return x end; print(update(3))'
        Output = 'local function update(x) return x + 1 end; print(update(3))'
    }
)
foreach ($check in $checks) {
    $before = Join-Path $results ($check.Name + '.lua')
    $after = Join-Path $results ($check.Name + '.out.lua')
    Set-Content -LiteralPath $before -Value $check.Source -Encoding utf8
    Set-Content -LiteralPath $after -Value $check.Output -Encoding utf8
    $diagnostic = (& $exe --check-sugar $before $after 2>&1) -join "`n"
    if ($LASTEXITCODE -ne $check.Exit -or ($check.Finding -and !$diagnostic.Contains($check.Finding))) {
        throw "Structural check failed: $($check.Name): $diagnostic"
    }
}
$bloated = Join-Path $results 'bloated.lua'
Set-Content -LiteralPath $bloated -Encoding utf8 -Value @'
local unused = 123456
local function neverCalled() print("dead") return 123456 end
local function update(t) t.value += 3 end
local t = {value=1}
update(t)
print(t.value)
'@
$minDir = Join-Path $results 'minimizer'
$minLog = (& $exe --minimize-file $bloated --minimize-budget 128 --out $minDir 2>&1) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "Minimizer failed: $minLog" }
$reduced = Get-Content -LiteralPath (Join-Path $minDir 'reproducer.min.lua') -Raw
if ($reduced.Length -ge (Get-Content -LiteralPath $bloated -Raw).Length -or $reduced.Contains('neverCalled')) {
    throw 'Minimizer did not remove unused statements'
}
Write-Output "PASS: 200 semantic matches; 15 sugar forms; function/target matching; return inlining; failure minimization. Report: $results/report.log"

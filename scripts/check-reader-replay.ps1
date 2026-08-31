param(
    [Parameter(Mandatory = $true)]
    [string]$TracePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$firstOutput = Join-Path $OutputRoot 'replay-first.jsonl'
$secondOutput = Join-Path $OutputRoot 'replay-second.jsonl'

& node scripts/run-reader-replay.cjs replay $TracePath --output $firstOutput
if ($LASTEXITCODE -ne 0) { throw 'First Replay execution failed' }
& node scripts/run-reader-replay.cjs replay $TracePath --output $secondOutput
if ($LASTEXITCODE -ne 0) { throw 'Second Replay execution failed' }

$firstHash = (Get-FileHash -LiteralPath $firstOutput -Algorithm SHA256).Hash
$secondHash = (Get-FileHash -LiteralPath $secondOutput -Algorithm SHA256).Hash
if ($firstHash -ne $secondHash) {
    throw 'Replay output is not deterministic for the same manifest and seed'
}

$records = Get-Content -LiteralPath $firstOutput | ForEach-Object { $_ | ConvertFrom-Json }
$cancelled = $records | Where-Object {
    $_.kind -eq 'job-result' -and $_.errorCode -eq 'CANCELLED'
}
if ($records[0].schemaVersion -ne 1 -or $cancelled.Count -ne 1) {
    throw 'Replay output does not contain the expected versioned cancellation result'
}

Write-Host "reader-replay deterministic cancellation passed: $firstHash"

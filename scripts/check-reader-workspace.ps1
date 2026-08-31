param(
    [Parameter(Mandatory = $true)]
    [string]$WorkspaceTool,

    [Parameter(Mandatory = $true)]
    [string]$WorkspaceRoot
)

$ErrorActionPreference = 'Stop'

function Invoke-WorkspaceCommand {
    param([string[]]$Arguments)

    $output = & $WorkspaceTool @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "reader-workspace failed with exit code $exitCode`: $output"
    }
    return ($output | ConvertFrom-Json)
}

$databasePath = Join-Path $WorkspaceRoot 'workspace.db'
$beforeHash = (Get-FileHash -LiteralPath $databasePath -Algorithm SHA256).Hash

$inspection = Invoke-WorkspaceCommand -Arguments @('inspect', $WorkspaceRoot)
if (-not $inspection.ok -or $inspection.schemaVersion -ne 2 -or $inspection.migrationRequired) {
    throw 'reader-workspace inspect returned an unexpected result'
}

$verification = Invoke-WorkspaceCommand -Arguments @('verify', $WorkspaceRoot)
if (-not $verification.ok -or -not $verification.valid -or $verification.documentCount -ne 1) {
    throw 'reader-workspace verify returned an unexpected result'
}

$migration = Invoke-WorkspaceCommand -Arguments @('migrate', $WorkspaceRoot, '--dry-run')
if (-not $migration.ok -or $migration.command -ne 'migrate-dry-run' -or $migration.migrationRequired) {
    throw 'reader-workspace migrate --dry-run returned an unexpected result'
}

$afterHash = (Get-FileHash -LiteralPath $databasePath -Algorithm SHA256).Hash
if ($beforeHash -ne $afterHash) {
    throw 'reader-workspace dry-run changed the workspace database'
}

Write-Host 'reader-workspace CLI checks passed'

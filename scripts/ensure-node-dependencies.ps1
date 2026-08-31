param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$lockPath = Join-Path $projectRoot 'package-lock.json'
$markerDirectory = Join-Path $projectRoot 'build'
$markerPath = Join-Path $markerDirectory 'node-dependencies.sha256'
$lockHash = (Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash
$requiredPaths = @(
    (Join-Path $projectRoot 'node_modules\.bin\cmake-js.cmd'),
    (Join-Path $projectRoot 'node_modules\.bin\electron.cmd'),
    (Join-Path $projectRoot 'node_modules\electron\package.json'),
    (Join-Path $projectRoot 'node_modules\node-addon-api\napi.h')
)

$installedHash = $null
if (Test-Path -LiteralPath $markerPath -PathType Leaf) {
    $installedHash = [IO.File]::ReadAllText($markerPath).Trim()
}

$installationComplete = $true
foreach ($requiredPath in $requiredPaths) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        $installationComplete = $false
        break
    }
}

if (-not $Force -and $installationComplete -and $installedHash -ceq $lockHash) {
    Write-Output 'Node dependencies already match package-lock.json; skipping npm ci.'
    exit 0
}

Push-Location $projectRoot
try {
    & npm ci --no-audit --no-fund
    if ($LASTEXITCODE -ne 0) {
        throw "npm ci failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

New-Item -ItemType Directory -Path $markerDirectory -Force | Out-Null
[IO.File]::WriteAllText($markerPath, "$lockHash`n")
Write-Output 'Node dependencies installed from package-lock.json.'

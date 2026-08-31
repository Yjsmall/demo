param(
    [Parameter(Mandatory = $true)]
    [string]$FacadeTool,

    [Parameter(Mandatory = $true)]
    [string]$FixturePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$workspacePath = Join-Path $OutputRoot ("facade-" + [guid]::NewGuid().ToString('N'))
$nodeOutput = Join-Path $OutputRoot 'node-boundary-contract.json'
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

$facadeText = & $FacadeTool $workspacePath $FixturePath
if ($LASTEXITCODE -ne 0) { throw "Facade contract failed: $facadeText" }
$facade = $facadeText | ConvertFrom-Json

& node scripts/run-electron-node-contract.cjs --output $nodeOutput
if ($LASTEXITCODE -ne 0) { throw 'Node contract failed' }
$node = Get-Content -LiteralPath $nodeOutput -Raw | ConvertFrom-Json

$properties = @(
    'runtimeApiVersion', 'workspaceSchemaVersion', 'documentCount', 'pngValid',
    'annotationCount', 'noteCount', 'noteRevision', 'workspaceValid',
    'closedDocumentCode', 'closedWorkspaceCode'
)
foreach ($property in $properties) {
    if ($facade.$property -ne $node.$property) {
        throw "Boundary contract differs at $property`: facade=$($facade.$property), node=$($node.$property)"
    }
}

Remove-Item -LiteralPath $workspacePath -Recurse -Force

Write-Host 'Facade and Node behavior contract passed'

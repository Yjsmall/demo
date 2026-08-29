$ErrorActionPreference = 'Stop'

$msysRoot = $env:CONTEXT_READER_MSYS2_ROOT
if (-not $msysRoot) {
    throw 'CONTEXT_READER_MSYS2_ROOT is not set. Run this check through Pixi.'
}

$objdumpPath = Join-Path $msysRoot 'ucrt64\bin\objdump.exe'
$modulePath = Join-Path $PSScriptRoot '..\build\node-ucrt64\reader_node.node'

if (-not (Test-Path -LiteralPath $objdumpPath)) {
    throw "objdump was not found: $objdumpPath"
}

if (-not (Test-Path -LiteralPath $modulePath)) {
    throw "reader_node was not found: $modulePath"
}

$headers = & $objdumpPath -p $modulePath
if ($LASTEXITCODE -ne 0) {
    throw "objdump failed with exit code $LASTEXITCODE"
}

$imports = @($headers | Select-String -Pattern '^\s*DLL Name:\s*(.+)$')
$importNames = @($imports | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() })

if ($importNames -notcontains 'electron.exe') {
    throw 'reader_node does not import Electron N-API symbols from electron.exe.'
}

if ($importNames -contains 'node.exe') {
    throw 'reader_node imports node.exe; this crashes when loaded by Electron under MinGW.'
}

Write-Output 'reader_node import boundary passed: electron.exe present, node.exe absent.'

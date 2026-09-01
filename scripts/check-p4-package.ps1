param(
    [string]$OutputRoot = 'out'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$root = Join-Path $projectRoot $OutputRoot
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "Forge output directory is missing: $root"
}

$packagedExe = Get-ChildItem -LiteralPath $root -Filter 'electron.exe' -File -Recurse |
    Where-Object { $_.FullName -match 'win32-x64' } |
    Select-Object -First 1
$nativeAddon = Get-ChildItem -LiteralPath $root -Filter 'reader_node.node' -File -Recurse |
    Where-Object { $_.FullName -match 'asar\.unpacked' } |
    Select-Object -First 1
$runtimeDll = Get-ChildItem -LiteralPath $root -Filter 'libwinpthread-1.dll' -File -Recurse |
    Where-Object { $_.FullName -match 'asar\.unpacked' } |
    Select-Object -First 1
$setup = Get-ChildItem -LiteralPath $root -Filter '*Setup.exe' -File -Recurse | Select-Object -First 1
$nupkg = Get-ChildItem -LiteralPath $root -Filter '*.nupkg' -File -Recurse | Select-Object -First 1
$releases = Get-ChildItem -LiteralPath $root -Filter 'RELEASES' -File -Recurse | Select-Object -First 1

if ($null -eq $packagedExe) { throw 'Packaged electron.exe was not produced.' }
if ($null -eq $nativeAddon) { throw 'reader_node.node was not unpacked from ASAR.' }
if ($null -eq $runtimeDll) { throw 'libwinpthread-1.dll was not unpacked beside the native addon.' }
if ($null -eq $setup -or $null -eq $nupkg -or $null -eq $releases) {
    throw 'Squirrel output must include Setup.exe, NUPKG and RELEASES.'
}

Write-Output "P4 package structure passed: $($setup.FullName)"

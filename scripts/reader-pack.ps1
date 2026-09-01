param(
    [string]$OutputDirectory = 'build/release'
)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$releaseRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build'))
if (-not $releaseRoot.StartsWith("$allowedRoot\", [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Release output must be a child of the project build directory.'
}

$squirrelRoot = Join-Path $projectRoot 'out\make\squirrel.windows\x64'
$nativeAddon = Join-Path $projectRoot 'build\node-p2-ucrt64\reader_node.node'
$benchmark = Join-Path $projectRoot 'build\p4-bench.json'
$buildIdPath = Join-Path $projectRoot 'build\build-id.txt'
$releaseNotes = Join-Path $projectRoot 'docs\release-notes-v0.1.md'
$required = @($squirrelRoot, $nativeAddon, $benchmark, $buildIdPath, $releaseNotes)
foreach ($path in $required) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required release input is missing: $path" }
}

if (Test-Path -LiteralPath $releaseRoot) {
    Remove-Item -LiteralPath $releaseRoot -Recurse -Force
}
$artifactsRoot = New-Item -ItemType Directory -Path (Join-Path $releaseRoot 'artifacts') -Force
$licensesRoot = New-Item -ItemType Directory -Path (Join-Path $releaseRoot 'licenses') -Force
$symbolsRoot = New-Item -ItemType Directory -Path (Join-Path $releaseRoot 'symbols') -Force

Get-ChildItem -LiteralPath $squirrelRoot -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $artifactsRoot.FullName
}
Copy-Item -LiteralPath $benchmark -Destination (Join-Path $releaseRoot 'performance.json')
Copy-Item -LiteralPath $releaseNotes -Destination (Join-Path $releaseRoot 'release-notes.md')

$licenseInputs = [ordered]@{
    'electron-LICENSE' = 'node_modules\electron\LICENSE'
    'markdown-it-LICENSE' = 'node_modules\markdown-it\LICENSE'
    'mdit-plugin-katex-LICENSE' = 'node_modules\@mdit\plugin-katex\LICENSE'
    'katex-LICENSE' = 'node_modules\katex\LICENSE'
    'dompurify-LICENSE' = 'node_modules\dompurify\LICENSE'
    'dompurify-LICENSE-MPL' = 'node_modules\dompurify\LICENSE-MPL'
    'lucide-LICENSE' = 'node_modules\lucide\LICENSE'
    'mupdf-COPYING' = 'build\mupdf-1.28.3\COPYING'
    'miniz-LICENSE' = 'build\node-p2-ucrt64\_deps\context_reader_miniz-src\LICENSE'
}
foreach ($entry in $licenseInputs.GetEnumerator()) {
    $source = Join-Path $projectRoot $entry.Value
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "License input is missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $licensesRoot.FullName $entry.Key)
}

$objcopy = Join-Path $env:CONTEXT_READER_MSYS2_ROOT 'ucrt64\bin\objcopy.exe'
if (-not (Test-Path -LiteralPath $objcopy -PathType Leaf)) {
    throw 'objcopy is unavailable; run reader-pack through Pixi.'
}
& $objcopy --only-keep-debug $nativeAddon (Join-Path $symbolsRoot.FullName 'reader_node.node.debug')
if ($LASTEXITCODE -ne 0) { throw "objcopy failed with exit code $LASTEXITCODE" }

$sbomPath = Join-Path $releaseRoot 'sbom.cdx.json'
$sbomLog = Join-Path $projectRoot 'build\cyclonedx-npm.stderr.log'
& (Join-Path $projectRoot 'node_modules\.bin\cyclonedx-npm.cmd') --package-lock-only --ignore-npm-errors --output-reproducible --output-file $sbomPath --output-format JSON 2> $sbomLog
$sbomExitCode = $LASTEXITCODE
if ($sbomExitCode -ne 0) {
    $sbomError = if (Test-Path -LiteralPath $sbomLog) { [IO.File]::ReadAllText($sbomLog) } else { '' }
    throw "CycloneDX generation failed with exit code ${sbomExitCode}: $sbomError"
}
Remove-Item -LiteralPath $sbomLog -Force -ErrorAction SilentlyContinue

$buildId = [IO.File]::ReadAllText($buildIdPath).Trim()
$testManifest = [ordered]@{
    version = 1
    buildId = $buildId
    target = 'win32-x64'
    qualificationCommand = 'pixi run p4'
    suites = @('P3 regression', 'P4 increments', 'benchmark smoke', 'packaged setup/recovery smoke', 'Squirrel structure')
    installerQualification = 'Requires a clean Windows runner before external release'
}
$testManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $releaseRoot 'test-manifest.json') -Encoding utf8

$manifestEntries = Get-ChildItem -LiteralPath $releaseRoot -File -Recurse |
    Where-Object { $_.Name -ne 'manifest.json' } |
    Sort-Object FullName |
    ForEach-Object {
        [ordered]@{
            path = [IO.Path]::GetRelativePath($releaseRoot, $_.FullName).Replace('\', '/')
            size = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
$manifest = [ordered]@{
    format = 'context-reader-release'
    version = 1
    buildId = $buildId
    target = 'win32-x64'
    files = @($manifestEntries)
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $releaseRoot 'manifest.json') -Encoding utf8

Write-Output "Release pack produced: $releaseRoot ($($manifest.files.Count) hashed files)"

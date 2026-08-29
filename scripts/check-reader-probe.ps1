param(
    [Parameter(Mandatory = $true)]
    [string]$ProbePath,

    [Parameter(Mandatory = $true)]
    [string]$CorpusRoot,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'

$validPdf = Join-Path $CorpusRoot 'generated\basic-rotated-cropbox.pdf'
$invalidPdf = Join-Path $CorpusRoot 'generated\corrupt-truncated.pdf'
$inspectOutput = Join-Path $OutputRoot 'inspect'
$renderOutput = Join-Path $OutputRoot 'render'
$textOutput = Join-Path $OutputRoot 'text'
$roundtripOutput = Join-Path $OutputRoot 'roundtrip'
New-Item -ItemType Directory -Path $inspectOutput -Force | Out-Null

$probeArguments = @('inspect', $validPdf, '--format', 'json', '--output', $inspectOutput)
$standardOutput = & $ProbePath @probeArguments
if ($LASTEXITCODE -ne 0) {
    throw "reader-probe inspect failed with exit code $LASTEXITCODE"
}

$stdoutDocument = ($standardOutput -join "`n") | ConvertFrom-Json
$documentPath = Join-Path $inspectOutput 'document.json'
$runManifestPath = Join-Path $inspectOutput 'run-manifest.json'
if (-not (Test-Path -LiteralPath $documentPath -PathType Leaf)) {
    throw 'reader-probe did not write document.json'
}
if (-not (Test-Path -LiteralPath $runManifestPath -PathType Leaf)) {
    throw 'reader-probe did not write run-manifest.json'
}

$document = Get-Content -LiteralPath $documentPath -Raw -Encoding UTF8 | ConvertFrom-Json
$runManifest = Get-Content -LiteralPath $runManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$expectedHash = (Get-FileHash -LiteralPath $validPdf -Algorithm SHA256).Hash.ToLowerInvariant()

if ($stdoutDocument.schema_version -ne 1 -or $document.schema_version -ne 1) {
    throw 'reader-probe document schema version mismatch'
}
if ($document.document.sha256 -cne $expectedHash -or $runManifest.input.document_sha256 -cne $expectedHash) {
    throw 'reader-probe document hash mismatch'
}
if ($document.document.page_count -ne 1 -or $document.pages.Count -ne 1) {
    throw 'reader-probe page count mismatch'
}
$page = $document.pages[0]
if ($page.width_points -ne 540 -or $page.height_points -ne 648 -or $page.rotation_degrees -ne 90) {
    throw 'reader-probe page metadata mismatch'
}
if ($runManifest.schema_version -ne 1 -or $runManifest.dependencies.mupdf -cne '1.28.3') {
    throw 'reader-probe run manifest version mismatch'
}

$unicodeDirectory = Join-Path $inspectOutput '路径'
$unicodePdf = Join-Path $unicodeDirectory '旋转页面.pdf'
New-Item -ItemType Directory -Path $unicodeDirectory -Force | Out-Null
Copy-Item -LiteralPath $validPdf -Destination $unicodePdf -Force
$unicodeArguments = @('inspect', $unicodePdf, '--format', 'json')
$unicodeOutput = & $ProbePath @unicodeArguments
if ($LASTEXITCODE -ne 0) {
    throw "reader-probe Unicode path failed with exit code $LASTEXITCODE"
}
$unicodeDocument = ($unicodeOutput -join "`n") | ConvertFrom-Json
if ($unicodeDocument.document.sha256 -cne $expectedHash) {
    throw 'reader-probe Unicode path document hash mismatch'
}

$invalidArguments = @('inspect', $invalidPdf, '--format', 'json')
& $ProbePath @invalidArguments 2>$null | Out-Null
if ($LASTEXITCODE -ne 4) {
    throw "reader-probe corrupt-input exit code was $LASTEXITCODE instead of 4"
}

$renderArguments = @(
    'render', $validPdf, '--page', '1', '--scale', '1', '--dpr', '1',
    '--format', 'json', '--output', $renderOutput
)
$renderStandardOutput = & $ProbePath @renderArguments
if ($LASTEXITCODE -ne 0) {
    throw "reader-probe render failed with exit code $LASTEXITCODE"
}
$renderOutputJson = ($renderStandardOutput -join "`n") | ConvertFrom-Json
$pngPath = Join-Path $renderOutput 'page-0001.png'
$renderJsonPath = Join-Path $renderOutput 'page-0001.render.json'
$renderManifestPath = Join-Path $renderOutput 'run-manifest.json'
foreach ($requiredPath in @($pngPath, $renderJsonPath, $renderManifestPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "reader-probe render did not write $requiredPath"
    }
}
$pngBytes = [System.IO.File]::ReadAllBytes($pngPath)
$expectedPngSignature = @(137, 80, 78, 71, 13, 10, 26, 10)
for ($index = 0; $index -lt $expectedPngSignature.Count; $index += 1) {
    if ($pngBytes[$index] -ne $expectedPngSignature[$index]) {
        throw 'reader-probe render output is not PNG'
    }
}
$pngHash = (Get-FileHash -LiteralPath $pngPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($renderOutputJson.width_pixels -ne 648 -or $renderOutputJson.height_pixels -ne 540) {
    throw 'reader-probe render dimensions mismatch'
}
if ($renderOutputJson.png.sha256 -cne $pngHash) {
    throw 'reader-probe render PNG hash mismatch'
}

$textArguments = @('text', $validPdf, '--page', '1', '--format', 'json', '--output', $textOutput)
$textStandardOutput = & $ProbePath @textArguments
if ($LASTEXITCODE -ne 0) {
    throw "reader-probe text failed with exit code $LASTEXITCODE"
}
$textOutputJson = ($textStandardOutput -join "`n") | ConvertFrom-Json
$layoutPath = Join-Path $textOutput 'page-0001.layout.json'
$plainTextPath = Join-Path $textOutput 'page-0001.text.txt'
$textManifestPath = Join-Path $textOutput 'run-manifest.json'
foreach ($requiredPath in @($layoutPath, $plainTextPath, $textManifestPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "reader-probe text did not write $requiredPath"
    }
}
if ($textOutputJson.text.Trim() -cne 'Context Reader P1' -or $textOutputJson.lines.Count -ne 1) {
    throw 'reader-probe extracted text mismatch'
}
$lineBounds = $textOutputJson.lines[0].bounds
if ($lineBounds.x -lt 0 -or $lineBounds.y -lt 0 -or
    $lineBounds.x + $lineBounds.width -gt 540 -or
    $lineBounds.y + $lineBounds.height -gt 648) {
    throw 'reader-probe text bounds are outside normalized CropBox coordinates'
}

$roundtripArguments = @(
    'roundtrip', $validPdf, '--page', '1', '--scale', '1.5', '--dpr', '2',
    '--format', 'json', '--output', $roundtripOutput
)
$roundtripStandardOutput = & $ProbePath @roundtripArguments
if ($LASTEXITCODE -ne 0) {
    throw "reader-probe roundtrip failed with exit code $LASTEXITCODE"
}
$roundtripJson = ($roundtripStandardOutput -join "`n") | ConvertFrom-Json
$coordinatesPath = Join-Path $roundtripOutput 'coordinates.jsonl'
$roundtripPath = Join-Path $roundtripOutput 'roundtrip.json'
$roundtripManifestPath = Join-Path $roundtripOutput 'run-manifest.json'
foreach ($requiredPath in @($coordinatesPath, $roundtripPath, $roundtripManifestPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "reader-probe roundtrip did not write $requiredPath"
    }
}
$coordinateRecords = @(Get-Content -LiteralPath $coordinatesPath -Encoding UTF8)
if (-not $roundtripJson.passed -or $roundtripJson.samples.Count -ne 5 -or
    $roundtripJson.maximum_error_points -gt $roundtripJson.tolerance_points -or
    $coordinateRecords.Count -ne 5) {
    throw 'reader-probe coordinate roundtrip contract mismatch'
}

$baselineDirectory = Join-Path $OutputRoot 'compare-baseline'
$actualDirectory = Join-Path $OutputRoot 'compare-actual'
New-Item -ItemType Directory -Path $baselineDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $actualDirectory -Force | Out-Null
foreach ($fileName in @('page-0001.layout.json', 'page-0001.text.txt', 'run-manifest.json')) {
    $sourcePath = Join-Path $textOutput $fileName
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $baselineDirectory $fileName) -Force
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $actualDirectory $fileName) -Force
}
Add-Content -LiteralPath (Join-Path $actualDirectory 'run-manifest.json') -Value ' ' -Encoding UTF8

$compareArguments = @('compare', $actualDirectory, $baselineDirectory, '--format', 'json')
$matchingStandardOutput = & $ProbePath @compareArguments
if ($LASTEXITCODE -ne 0) {
    throw "reader-probe matching comparison failed with exit code $LASTEXITCODE"
}
$matchingComparison = ($matchingStandardOutput -join "`n") | ConvertFrom-Json
if ($matchingComparison.status -cne 'passed' -or $matchingComparison.changed.Count -ne 0) {
    throw 'reader-probe matching comparison contract mismatch'
}

Add-Content -LiteralPath (Join-Path $actualDirectory 'page-0001.text.txt') -Value 'changed' -Encoding UTF8
$changedStandardOutput = & $ProbePath @compareArguments
if ($LASTEXITCODE -ne 6) {
    throw "reader-probe changed comparison exit code was $LASTEXITCODE instead of 6"
}
$changedComparison = ($changedStandardOutput -join "`n") | ConvertFrom-Json
if ($changedComparison.status -cne 'failed' -or $changedComparison.changed.Count -ne 1 -or
    $changedComparison.changed[0].path -cne 'page-0001.text.txt') {
    throw 'reader-probe changed comparison contract mismatch'
}

Write-Output 'reader-probe inspect/render/text/roundtrip/compare contracts passed.'

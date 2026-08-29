$ErrorActionPreference = 'Stop'

$muPdfVersion = '1.28.3'
$muPdfCommit = 'e85b44bee98e322a81d91be2535c2b089f74ebb4'
$muPdfRepository = 'https://github.com/ArtifexSoftware/mupdf.git'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$muPdfRoot = Join-Path $projectRoot "build\mupdf-$muPdfVersion"
$muPdfOutput = Join-Path $muPdfRoot 'build\context-reader-release'

$msysRoot = $env:CONTEXT_READER_MSYS2_ROOT
if (-not $msysRoot) {
    throw 'CONTEXT_READER_MSYS2_ROOT is not set. Run this script through Pixi.'
}

$requiredTools = @{
    Make = Join-Path $msysRoot 'usr\bin\make.exe'
    Shell = Join-Path $msysRoot 'usr\bin\sh.exe'
    Cygpath = Join-Path $msysRoot 'usr\bin\cygpath.exe'
    Clang = Join-Path $msysRoot 'ucrt64\bin\clang.exe'
    ClangXX = Join-Path $msysRoot 'ucrt64\bin\clang++.exe'
    Ar = Join-Path $msysRoot 'ucrt64\bin\ar.exe'
    Ranlib = Join-Path $msysRoot 'ucrt64\bin\ranlib.exe'
    PkgConfig = Join-Path $msysRoot 'ucrt64\bin\pkg-config.exe'
}
foreach ($entry in $requiredTools.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) {
        throw "MuPDF build prerequisite '$($entry.Key)' was not found: $($entry.Value)"
    }
}

if (-not (Test-Path -LiteralPath $muPdfRoot)) {
    & git clone --branch $muPdfVersion --depth 1 --recurse-submodules --shallow-submodules `
        $muPdfRepository $muPdfRoot
    if ($LASTEXITCODE -ne 0) {
        throw "MuPDF clone failed with exit code $LASTEXITCODE"
    }
} else {
    if (-not (Test-Path -LiteralPath (Join-Path $muPdfRoot '.git'))) {
        throw "MuPDF root exists but is not a Git checkout: $muPdfRoot"
    }
    & git -C $muPdfRoot submodule update --init --recursive --depth 1
    if ($LASTEXITCODE -ne 0) {
        throw "MuPDF submodule update failed with exit code $LASTEXITCODE"
    }
}

$actualCommit = (& git -C $muPdfRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -cne $muPdfCommit) {
    throw "MuPDF commit mismatch: expected $muPdfCommit, found $actualCommit"
}

$versionHeader = Join-Path $muPdfRoot 'include\mupdf\fitz\version.h'
$versionText = Get-Content -LiteralPath $versionHeader -Raw -Encoding UTF8
if ($versionText -notmatch '#define FZ_VERSION "1\.28\.3"') {
    throw "MuPDF header does not report version $muPdfVersion"
}

$msysMuPdfRoot = (& $requiredTools.Cygpath -u $muPdfRoot).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'cygpath failed to convert the MuPDF source path.'
}

$makeArguments = @(
    '-j4',
    '-C', $msysMuPdfRoot,
    'libs',
    'OUT=build/context-reader-release',
    'build=release',
    'OS=mingw',
    'CC=/ucrt64/bin/clang',
    'CXX=/ucrt64/bin/clang++',
    'AR=/ucrt64/bin/ar',
    'RANLIB=/ucrt64/bin/ranlib',
    'HAVE_GLUT=no',
    'HAVE_X11=no',
    'HAVE_LIBCRYPTO=no',
    'threading=no',
    'html=no',
    'xps=no',
    'svg=no',
    'extract=no',
    'mujs=no',
    'brotli=no',
    'barcode=no',
    'tesseract=no',
    'XCFLAGS=-DARCH_HAS_SSE=0'
)

$originalPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $msysRoot 'ucrt64\bin');$(Join-Path $msysRoot 'usr\bin');$originalPath"
    & $requiredTools.Make @makeArguments
    if ($LASTEXITCODE -ne 0) {
        throw "MuPDF build failed with exit code $LASTEXITCODE"
    }
} finally {
    $env:PATH = $originalPath
}

foreach ($libraryName in @('libmupdf.a', 'libmupdf-third.a')) {
    $libraryPath = Join-Path $muPdfOutput $libraryName
    if (-not (Test-Path -LiteralPath $libraryPath -PathType Leaf)) {
        throw "MuPDF build did not produce $libraryPath"
    }
}

Write-Output "MuPDF $muPdfVersion prepared at commit $muPdfCommit"

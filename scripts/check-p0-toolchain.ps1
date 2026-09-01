$ErrorActionPreference = 'Stop'

function Invoke-VersionCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $output = & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Executable failed with exit code $LASTEXITCODE"
    }

    return ($output | Select-Object -First 1).Trim()
}

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Actual,

        [Parameter(Mandatory = $true)]
        [string]$Expected
    )

    if ($Actual -cne $Expected) {
        throw "$Name version mismatch: expected $Expected, found $Actual"
    }
}

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$msysRoot = $env:CONTEXT_READER_MSYS2_ROOT
if (-not $msysRoot) {
    throw 'CONTEXT_READER_MSYS2_ROOT is not set. Run this check through Pixi.'
}

$compilerPreference = $env:CONTEXT_READER_COMPILER
if (-not $compilerPreference) {
    $compilerPreference = 'auto'
}
if ($compilerPreference -notin @('auto', 'clang', 'gcc')) {
    throw 'CONTEXT_READER_COMPILER must be auto, clang, or gcc.'
}

$clangPath = Join-Path $msysRoot 'ucrt64\bin\clang++.exe'
$gccPath = Join-Path $msysRoot 'ucrt64\bin\g++.exe'
if (($compilerPreference -eq 'auto' -or $compilerPreference -eq 'clang') -and
    (Test-Path -LiteralPath $clangPath -PathType Leaf)) {
    $compilerName = 'Clang'
    $compilerPath = $clangPath
    $compilerVersionLine = Invoke-VersionCommand -Executable $compilerPath -Arguments @('--version')
    if ($compilerVersionLine -notmatch '^clang version ([0-9]+\.[0-9]+\.[0-9]+)') {
        throw "Unexpected Clang version output: $compilerVersionLine"
    }
    $compilerVersion = $Matches[1]
    $expectedCompilerVersion = '22.1.8'

    $linkerPath = Join-Path $msysRoot 'ucrt64\bin\ld.lld.exe'
    if (-not (Test-Path -LiteralPath $linkerPath -PathType Leaf)) {
        throw "UCRT64 lld was not found: $linkerPath"
    }
    $linkerVersionLine = Invoke-VersionCommand -Executable $linkerPath -Arguments @('--version')
    if ($linkerVersionLine -notmatch '^LLD ([0-9]+\.[0-9]+\.[0-9]+)') {
        throw "Unexpected lld version output: $linkerVersionLine"
    }
    $linkerVersion = $Matches[1]
    Assert-Equal -Name 'lld' -Actual $linkerVersion -Expected '22.1.8'
} elseif ($compilerPreference -ne 'clang' -and (Test-Path -LiteralPath $gccPath -PathType Leaf)) {
    $compilerName = 'GCC fallback'
    $compilerPath = $gccPath
    $compilerVersion = Invoke-VersionCommand -Executable $compilerPath -Arguments @('-dumpfullversion')
    $expectedCompilerVersion = '16.2.0'
} else {
    throw "The requested UCRT64 compiler '$compilerPreference' was not found under $msysRoot."
}

$cmakeCommand = Get-Command cmake -CommandType Application -ErrorAction Stop |
    Select-Object -First 1
$ninjaCommand = Get-Command ninja -CommandType Application -ErrorAction Stop |
    Select-Object -First 1
$nodeCommand = Get-Command node -CommandType Application -ErrorAction Stop |
    Select-Object -First 1

$cmakeVersionLine = Invoke-VersionCommand -Executable $cmakeCommand.Source -Arguments @('--version')
$ninjaVersion = Invoke-VersionCommand -Executable $ninjaCommand.Source -Arguments @('--version')
$nodeVersion = Invoke-VersionCommand -Executable $nodeCommand.Source -Arguments @('--version')

if ($cmakeVersionLine -notmatch '^cmake version ([0-9]+\.[0-9]+\.[0-9]+)$') {
    throw "Unexpected CMake version output: $cmakeVersionLine"
}
$cmakeVersion = $Matches[1]

Assert-Equal -Name $compilerName -Actual $compilerVersion -Expected $expectedCompilerVersion
Assert-Equal -Name 'CMake' -Actual $cmakeVersion -Expected '3.31.8'
Assert-Equal -Name 'Ninja' -Actual $ninjaVersion -Expected '1.13.2'
Assert-Equal -Name 'Development Node.js' -Actual $nodeVersion -Expected 'v24.19.0'

$packagePath = Join-Path $projectRoot 'package.json'
$package = Get-Content -LiteralPath $packagePath -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-Equal -Name 'Electron package' -Actual $package.devDependencies.electron -Expected '44.1.0'
Assert-Equal -Name 'CMake.js package' -Actual $package.devDependencies.'cmake-js' -Expected '8.0.0'

Write-Output (
    'P0 toolchain boundary passed: {0} {1}{2}, CMake {3}, Ninja {4}, Node.js {5}, Electron {6}, CMake.js {7}.' -f
        $compilerName,
        $compilerVersion,
        $(if ($linkerVersion) { " / lld $linkerVersion" } else { '' }),
        $cmakeVersion,
        $ninjaVersion,
        $nodeVersion.TrimStart('v'),
        $package.devDependencies.electron,
        $package.devDependencies.'cmake-js'
)

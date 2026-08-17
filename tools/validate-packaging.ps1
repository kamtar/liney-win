# validate-packaging.ps1 — static packaging contract checks.
#
# This intentionally does not configure, build, invoke NSIS, or create an
# installer. It catches artifact-name/path drift before a release job spends
# time producing packages.

param([string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $RepositoryRoot).Path

function Require-File([string]$RelativePath) {
    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required packaging file is missing: $RelativePath"
    }
    return $path
}

function Require-Match([string]$Text, [string]$Pattern, [string]$Description) {
    if ($Text -notmatch $Pattern) {
        throw "Packaging contract failed: $Description"
    }
}

$makeInstallerPath = Require-File 'tools\make-installer.ps1'
$nsiPath = Require-File 'packaging\liney-win.nsi'
$releasePath = Require-File '.github\workflows\release.yml'
$portablePath = Require-File 'tools\make-portable.ps1'

$makeInstaller = Get-Content -LiteralPath $makeInstallerPath -Raw
$nsi = Get-Content -LiteralPath $nsiPath -Raw
$release = Get-Content -LiteralPath $releasePath -Raw
$portable = Get-Content -LiteralPath $portablePath -Raw

# The lowercase name is the public artifact name used by updates, checksums,
# release uploads, and installer smoke tests. Keep the direct-NSIS fallback in
# sync with the explicit /DOUTFILE passed by make-installer.ps1.
Require-Match $makeInstaller '\$out\s*=\s*Join-Path\s+\$dist\s+[''\"]liney-setup\.exe[''\"]' `
    'make-installer.ps1 writes dist\liney-setup.exe'
Require-Match $makeInstaller '"/DOUTFILE=\$out"' `
    'make-installer.ps1 passes its exact output path to NSIS'
Require-Match $makeInstaller 'Test-Path\s+-LiteralPath\s+\$out\s+-PathType\s+Leaf' `
    'make-installer.ps1 verifies the exact NSIS output path'
Require-Match $nsi '!define\s+OUTFILE\s+"liney-setup\.exe"' `
    'NSIS direct-invocation fallback uses the release artifact name'
Require-Match $nsi 'OutFile\s+"\$\{OUTFILE\}"' `
    'NSIS uses the configured output path'

foreach ($define in @('WINEXE', 'GHOSTTYDLL', 'BINDIR', 'ICONFILE')) {
    Require-Match $makeInstaller ("/D$define=") `
        "make-installer.ps1 passes /D$define to NSIS"
    Require-Match $nsi ("\$\{" + $define + "\}") `
        "NSIS consumes /D$define"
}

Require-Match $release 'dist[\\/]liney-setup\.exe' `
    'release workflow references dist\liney-setup.exe'
Require-Match $release 'dist[\\/]liney-portable\.zip' `
    'release workflow references dist\liney-portable.zip'
Require-Match $portable '\$zip\s*=\s*Join-Path\s+\$dist\s+[''\"]liney-portable\.zip[''\"]' `
    'portable packager writes dist\liney-portable.zip'

$staleSetupReferences = @(
    (rg -n --fixed-strings ('liney-' + 'Setup.exe') $root 2>$null)
)
if ($staleSetupReferences.Count -gt 0) {
    throw "Packaging contract failed: stale case-variant setup name found:`n$($staleSetupReferences -join "`n")"
}

Write-Host 'Packaging contract passed: dist\liney-setup.exe and dist\liney-portable.zip are consistent.'

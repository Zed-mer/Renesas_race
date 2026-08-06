[CmdletBinding()]
param(
    [string] $OutputDirectory,
    [string] $Label = '20260730'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $root 'delivery'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$packageName = "RA8P1_DRONE_DETECT_SDR_PERSISTENT_$Label"
$stage = Join-Path $OutputDirectory $packageName
$archive = Join-Path $OutputDirectory ($packageName + '.zip')
$archiveHash = $archive + '.sha256'

foreach ($path in @($stage, $archive, $archiveHash)) {
    if (Test-Path -LiteralPath $path) {
        throw "Delivery path already exists: $path"
    }
}

New-Item -ItemType Directory -Path $stage -Force | Out-Null

function Copy-RequiredFile {
    param([string] $Source, [string] $Destination)
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required package input is missing: $Source"
    }
    $parent = Split-Path -Parent $Destination
    if ($parent) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination
}

function Copy-FilteredDirectory {
    param(
        [string] $Source,
        [string] $Destination,
        [string[]] $SkipDirectoryNames = @(),
        [string[]] $SkipExtensions = @(),
        [string[]] $SkipNames = @()
    )
    $sourceRoot = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
    Get-ChildItem -LiteralPath $sourceRoot -File -Force -Recurse | ForEach-Object {
        $relative = $_.FullName.Substring($sourceRoot.Length).TrimStart('\')
        $components = $relative -split '[\\/]'
        if (@($components | Where-Object { $SkipDirectoryNames -contains $_ }).Count -gt 0) {
            return
        }
        if (($SkipExtensions -contains $_.Extension.ToLowerInvariant()) -or
            ($SkipNames -contains $_.Name)) {
            return
        }
        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target
    }
}

Copy-RequiredFile (Join-Path $root 'DELIVERY_README_20260730.md') (Join-Path $stage 'README.md')
foreach ($name in @(
    '.gitignore', '.project', '.secure_xml', 'solution.xml',
    'build-solution.ps1', 'flash-solution.ps1', 'verify-solution.ps1',
    'INTEGRATION_GUIDE_CN.md', 'OPTIMIZATION_DESIGN_20260726.md',
    'PROJECT_HANDOFF_20260725.md', 'PROVENANCE.md'
)) {
    Copy-RequiredFile (Join-Path $root $name) (Join-Path $stage $name)
}
Copy-RequiredFile (Join-Path $root 'README.md') (Join-Path $stage 'docs\README_repository_snapshot.md')
Copy-RequiredFile (Join-Path $root 'PACKAGE_INFO_CURRENT.txt') (Join-Path $stage 'docs\PACKAGE_INFO_20260729_PRE_PERSISTENCE.txt')

$projectSkips = @('Debug', 'Release', '.git', '.metadata', '.pytest_cache', '__pycache__')
Copy-FilteredDirectory (Join-Path $root 'cpu0') (Join-Path $stage 'cpu0') -SkipDirectoryNames $projectSkips
Copy-FilteredDirectory (Join-Path $root 'cpu1') (Join-Path $stage 'cpu1') -SkipDirectoryNames $projectSkips
Copy-FilteredDirectory (Join-Path $root 'shared') (Join-Path $stage 'shared') -SkipDirectoryNames @('.git', '__pycache__')
Copy-FilteredDirectory (Join-Path $root '算法设计') (Join-Path $stage '算法设计') -SkipDirectoryNames @('__pycache__', '.pytest_cache')
Copy-FilteredDirectory (Join-Path $root 'ui-preview') (Join-Path $stage 'ui-preview') -SkipDirectoryNames @('__pycache__')
Copy-FilteredDirectory (Join-Path $root 'build') (Join-Path $stage 'build') -SkipDirectoryNames @('Debug', 'Release')

$toolSkipExtensions = @('.exe', '.o', '.obj', '.pyc', '.zip', '.gz', '.uue', '.armhf', '.elf', '.map')
Copy-FilteredDirectory (Join-Path $root 'tools') (Join-Path $stage 'tools') `
    -SkipDirectoryNames @('__pycache__', '.pytest_cache') -SkipExtensions $toolSkipExtensions
foreach ($name in @('sdr_libdl_glibc_225.map', 'sdr_libpthread_glibc_225.map')) {
    Copy-RequiredFile (Join-Path $root "tools\$name") (Join-Path $stage "tools\$name")
}

Copy-RequiredFile (Join-Path $root 'cpu0\Debug\rtthread.elf') (Join-Path $stage 'artifacts\ra8p1\cpu0\rtthread.elf')
Copy-RequiredFile (Join-Path $root 'cpu0\Debug\rtthread.map') (Join-Path $stage 'artifacts\ra8p1\cpu0\rtthread.map')
Copy-RequiredFile (Join-Path $root 'cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf') `
    (Join-Path $stage 'artifacts\ra8p1\cpu1\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf')
Copy-RequiredFile (Join-Path $root 'cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.map') `
    (Join-Path $stage 'artifacts\ra8p1\cpu1\ra8p1_sdr_ai_display_solution_20260718_CPU1.map')
$releaseManifest = Join-Path $root ("artifacts\ra8p1\{0}.sha256" -f $Label)
if (Test-Path -LiteralPath $releaseManifest -PathType Leaf) {
    Copy-RequiredFile $releaseManifest `
        (Join-Path $stage ("artifacts\ra8p1\{0}.sha256" -f $Label))
}

Copy-FilteredDirectory (Join-Path $root 'artifacts\sdr\persistent_current') `
    (Join-Path $stage 'artifacts\sdr\persistent_current')
Copy-FilteredDirectory (Join-Path $root 'evidence\sdr_persistent_20260730') `
    (Join-Path $stage 'evidence\sdr_persistent_20260730')
Copy-FilteredDirectory (Join-Path $root 'evidence\tune_guard_e2e_20260729') `
    (Join-Path $stage 'evidence\tune_guard_e2e_20260729')

$head = (& git -C $root rev-parse HEAD).Trim()
$status = @(& git -C $root status --short)
$packageInfo = @(
    "Package: $packageName",
    "Created: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')",
    "Source Git HEAD: $head",
    'Source kind: current worktree snapshot (tracked and untracked source included)',
    'RA8P1: CPU0 + CPU1 source, current ELF/MAP and build/flash/verify scripts',
    'SDR: content-addressed persistent agent/adapter, supervisor, installer and rollback',
    'Excluded: .git, Debug/Release intermediates, caches, old delivery archives and raw IQ'
)
[IO.File]::WriteAllLines((Join-Path $stage 'PACKAGE_INFO.txt'), $packageInfo, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllLines((Join-Path $stage 'WORKTREE_STATUS.txt'), $status, [Text.UTF8Encoding]::new($false))

$manifestPath = Join-Path $stage 'MANIFEST.sha256'
$manifestLines = Get-ChildItem -LiteralPath $stage -File -Force -Recurse |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length).TrimStart('\').Replace('\', '/')
        '{0} *{1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToUpperInvariant(), $relative
    }
[IO.File]::WriteAllLines($manifestPath, $manifestLines, [Text.UTF8Encoding]::new($true))

& (Join-Path $stage 'tools\verify-share-package.ps1') -Root $stage | Out-Null
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$digest = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToUpperInvariant()
[IO.File]::WriteAllText($archiveHash, "$digest *$packageName.zip`n", [Text.Encoding]::ASCII)

[pscustomobject]@{
    Status = 'created'
    Stage = $stage
    Archive = $archive
    ArchiveBytes = (Get-Item -LiteralPath $archive).Length
    ArchiveSha256 = $digest
    Manifest = $manifestPath
    ManifestFiles = $manifestLines.Count
}

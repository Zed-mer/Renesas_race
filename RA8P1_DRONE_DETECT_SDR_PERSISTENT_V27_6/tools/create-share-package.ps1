[CmdletBinding()]
param(
    [string] $OutputDirectory,
    [string] $Label = '20260725'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
$layoutHelper = Join-Path $PSScriptRoot 'project-layout.ps1'
if (-not (Test-Path -LiteralPath $layoutHelper -PathType Leaf))
{
    throw "Project layout helper not found: $layoutHelper"
}
. $layoutHelper
$layout = Resolve-Ra8p1ProjectLayout -Solution $root
if (-not $OutputDirectory)
{
    $OutputDirectory = Join-Path $root 'delivery'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

$packageName = "RA8P1_SDR_DRONE_SHARE_$Label"
$stage = Join-Path $OutputDirectory $packageName
$zip = Join-Path $OutputDirectory ($packageName + '.zip')
$zipHash = $zip + '.sha256'
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip) -or
    (Test-Path -LiteralPath $zipHash))
{
    throw "Delivery path already exists: $packageName. Choose another -Label; this script never overwrites a prior package."
}

New-Item -ItemType Directory -Path $stage -Force | Out-Null

function Copy-FilteredDirectory
{
    param(
        [Parameter(Mandatory = $true)] [string] $Source,
        [Parameter(Mandatory = $true)] [string] $Destination,
        [string[]] $SkipDirectoryNames = @(),
        [string[]] $SkipExtensions = @(),
        [string[]] $SkipNames = @()
    )

    $sourceRoot = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
    Get-ChildItem -LiteralPath $sourceRoot -File -Force -Recurse | ForEach-Object {
        $relative = $_.FullName.Substring($sourceRoot.Length).TrimStart('\')
        $components = $relative -split '[\\/]'
        if (@($components | Where-Object { $SkipDirectoryNames -contains $_ }).Count -gt 0)
        {
            return
        }
        if (($SkipExtensions -contains $_.Extension.ToLowerInvariant()) -or
            ($SkipNames -contains $_.Name))
        {
            return
        }

        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target
    }
}

function Copy-RequiredFile
{
    param(
        [Parameter(Mandatory = $true)] [string] $Source,
        [Parameter(Mandatory = $true)] [string] $Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf))
    {
        throw "Required package input is missing: $Source"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination
}

# Current hand-off documentation becomes the package root documentation.  The
# older root README is intentionally put under historical/ because it contains
# obsolete pre-SDR statements.
Copy-RequiredFile (Join-Path $root 'SHARE_PACKAGE_README_20260725.md') (Join-Path $stage 'README.md')
Copy-RequiredFile (Join-Path $root 'PROJECT_HANDOFF_20260725.md') (Join-Path $stage 'PROJECT_HANDOFF_20260725.md')
foreach ($name in @('build-solution.ps1', 'flash-solution.ps1', 'verify-solution.ps1',
                     'solution.xml', '.project', '.secure_xml', '.gitignore', 'PROVENANCE.md'))
{
    Copy-RequiredFile (Join-Path $root $name) (Join-Path $stage $name)
}
Copy-RequiredFile (Join-Path $root 'README.md') (Join-Path $stage 'historical\README_legacy.md')
Copy-RequiredFile (Join-Path $root 'PROJECT_HANDOFF_20260723.md') (Join-Path $stage 'historical\PROJECT_HANDOFF_20260723.md')
Copy-RequiredFile (Join-Path $root 'SYSTEM_ARCHITECTURE.md') (Join-Path $stage 'historical\SYSTEM_ARCHITECTURE.md')

# Source trees retain FSP configuration/generated content but omit compiler
# output and local caches.  The exact share-time binaries go under artifacts/.
$projectSkipDirs = @('Debug', 'Release', '.git', '.metadata', '.pytest_cache', '__pycache__')
Copy-FilteredDirectory $layout.Cpu0Directory `
    (Join-Path $stage 'cpu0') -SkipDirectoryNames $projectSkipDirs
Copy-FilteredDirectory $layout.Cpu1Directory `
    (Join-Path $stage 'cpu1') -SkipDirectoryNames $projectSkipDirs
Copy-FilteredDirectory (Join-Path $root 'shared') (Join-Path $stage 'shared') `
    -SkipDirectoryNames @('.git', '__pycache__')

$toolSkipExtensions = @('.exe', '.o', '.obj', '.pyc', '.zip', '.gz', '.uue', '.armhf', '.elf', '.map')
Copy-FilteredDirectory (Join-Path $root 'tools') (Join-Path $stage 'tools') `
    -SkipDirectoryNames @('__pycache__', '.pytest_cache') -SkipExtensions $toolSkipExtensions
foreach ($name in @('sdr_libdl_glibc_225.map', 'sdr_libpthread_glibc_225.map'))
{
    Copy-RequiredFile (Join-Path $PSScriptRoot $name) (Join-Path $stage ("tools\$name"))
}
Copy-RequiredFile $layout.SolutionBundle `
    (Join-Path $stage ("build\{0}.sbd" -f $layout.SolutionProjectName))

# Share-time RA8P1 artifacts.  They are useful for matching board snapshots,
# but a recipient must rebuild and re-hash after any source change.
Copy-RequiredFile $layout.Cpu0Elf `
    (Join-Path $stage 'artifacts\ra8p1\cpu0\rtthread.elf')
Copy-RequiredFile $layout.Cpu0Map `
    (Join-Path $stage 'artifacts\ra8p1\cpu0\rtthread.map')
Copy-RequiredFile $layout.Cpu1Elf `
    (Join-Path $stage 'artifacts\ra8p1\cpu1\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf')
Copy-RequiredFile $layout.Cpu1Map `
    (Join-Path $stage 'artifacts\ra8p1\cpu1\ra8p1_sdr_ai_display_solution_20260718_CPU1.map')

# The mmap pair is the documented reference pair.  The pacer candidate is
# retained separately for controlled A/B work and is deliberately not labeled
# as a paired mmap deployment.
Copy-RequiredFile (Join-Path $root 'tmp\build_capture_agent_armhf\sdr_capture_agent') `
    (Join-Path $stage 'artifacts\sdr\sdr_capture_agent_0d86a1d5')
Copy-RequiredFile (Join-Path $root 'tmp\build_capture_agent_armhf\sdr_adapter_iio_mmap.so') `
    (Join-Path $stage 'artifacts\sdr\sdr_adapter_iio_mmap_f2b9cfe1.so')
Copy-RequiredFile (Join-Path $root 'tmp\build_capture_agent_armhf_pacerfix\sdr_capture_agent') `
    (Join-Path $stage 'artifacts\sdr\candidates\sdr_capture_agent_2ca99815_pacerfix')
Copy-RequiredFile (Join-Path $root 'tmp\build_capture_agent_armhf_pacerfix\sdr_adapter_libiio.so') `
    (Join-Path $stage 'artifacts\sdr\candidates\sdr_adapter_libiio_pacerfix.so')

# Include a compact, explicitly failed formal report so its numbers retain
# their evidence boundary.  Do not copy gigabytes of temporary build/cache data.
$evidenceSource = Join-Path $root 'build\evidence\control_mailbox_500_20260725\overlap-four-center-10'
foreach ($name in @('verification_report.md', 'verification_report.json', 'manifest.json'))
{
    Copy-RequiredFile (Join-Path $evidenceSource $name) (Join-Path $stage ('evidence\overlap-four-center-10\' + $name))
}

$timestamp = @(
    "Package: $packageName",
    "Created: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')",
    'Scope: source + reproducible scripts + current binary snapshot + compact evidence',
    'Excluded: .git, IDE/cache files, Debug objects, tmp work trees, toolchains, SDR firmware, raw IQ captures'
)
Set-Content -LiteralPath (Join-Path $stage 'PACKAGE_INFO.txt') -Value $timestamp -Encoding utf8

$manifestPath = Join-Path $stage 'MANIFEST.sha256'
$manifestLines = Get-ChildItem -LiteralPath $stage -File -Force -Recurse |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length).TrimStart('\').Replace('\', '/')
        '{0} *{1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToUpperInvariant(), $relative
    }
Set-Content -LiteralPath $manifestPath -Value $manifestLines -Encoding ascii

# Verify before compression, then produce the archive and its detached hash.
& (Join-Path $stage 'tools\verify-share-package.ps1') -Root $stage | Out-Null
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
$zipDigest = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToUpperInvariant()
Set-Content -LiteralPath $zipHash -Value ("{0} *{1}" -f $zipDigest, (Split-Path -Leaf $zip)) -Encoding ascii

[pscustomobject]@{
    Status = 'created'
    Stage = $stage
    Archive = $zip
    ArchiveBytes = (Get-Item -LiteralPath $zip).Length
    ArchiveSha256 = $zipDigest
    Manifest = $manifestPath
}

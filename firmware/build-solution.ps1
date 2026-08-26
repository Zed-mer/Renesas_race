[CmdletBinding()]
param(
    [string] $E2Root = 'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0',
    [ValidateRange(1, 32)] [int] $Jobs = 8,
    [string] $Workspace,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$solution = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$layoutHelper = Join-Path $solution 'tools\project-layout.ps1'
$skillScripts = Join-Path $HOME '.codex\skills\ra8p1\scripts'
$dualBuild = Join-Path $skillScripts 'ra8p1-dual-build.ps1'
$bundleGenerator = Join-Path $skillScripts 'ease\regenerate_solution_bundle.py'
$e2Cli = Join-Path $E2Root 'eclipse\e2studio-cli.exe'
$shared = Join-Path $solution 'shared'
$sharedAbiGuard = Join-Path $solution 'tools\shared-abi-guard.ps1'
$crossCoreAbiVerifier = Join-Path $solution 'tools\verify-cross-core-abi.ps1'

foreach ($path in @($layoutHelper, $shared, $sharedAbiGuard, $crossCoreAbiVerifier,
                    $dualBuild, $bundleGenerator, $e2Cli, $E2Root))
{
    if (-not (Test-Path -LiteralPath $path))
    {
        throw "Required build path not found: $path"
    }
}

. $layoutHelper
. $sharedAbiGuard
$layout = Resolve-Ra8p1ProjectLayout -Solution $solution
$cpu0 = $layout.Cpu0Directory
$cpu1 = $layout.Cpu1Directory

if ($PSBoundParameters.ContainsKey('Jobs'))
{
    Write-Warning '-Jobs is retained for command-line compatibility; e2 studio controls parallelism for the official DDSC build.'
}

if (-not $Workspace)
{
    $Workspace = Join-Path $env:LOCALAPPDATA `
        ('Codex\ra8p1-dual-share-{0}' -f [Guid]::NewGuid().ToString('N'))
}
$Workspace = [IO.Path]::GetFullPath($Workspace)
$solutionPrefix = $solution.TrimEnd('\') + '\'
if ($Workspace.StartsWith($solutionPrefix, [StringComparison]::OrdinalIgnoreCase))
{
    throw "The e2 studio workspace must be outside the Solution directory: $Workspace"
}

if ($Clean)
{
    foreach ($project in @($cpu0, $cpu1))
    {
        $debug = [IO.Path]::GetFullPath((Join-Path $project 'Debug'))
        $projectPrefix = ([IO.Path]::GetFullPath($project)).TrimEnd('\') + '\'
        if (-not $debug.StartsWith($projectPrefix, [StringComparison]::OrdinalIgnoreCase))
        {
            throw "Refusing to clean outside the resolved child project: $debug"
        }
        if (Test-Path -LiteralPath $debug -PathType Container)
        {
            Remove-Item -LiteralPath $debug -Recurse -Force
        }
    }
}

$cpu0SeedBundle = Join-Path $cpu0 ("Debug\{0}.sbd" -f $layout.Cpu0ProjectName)
if (-not (Test-Path -LiteralPath $cpu0SeedBundle -PathType Leaf))
{
    # Child content generation reads the CPU0 SmartBundle before it can emit a
    # replacement.  Share packages omit Debug, so regenerate the authoritative
    # parent Solution bundle and seed the first CPU0 build from it.
    $bundleWorkspace = $Workspace + '-bundle'
    New-Item -ItemType Directory -Path $bundleWorkspace -Force | Out-Null
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $bundleOutput = @(& $e2Cli -data $bundleWorkspace ease run `
        -engine org.eclipse.ease.lang.python.py4j.engine `
        -script $bundleGenerator $solution 2>&1)
    $bundleExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorAction
    $bundleText = $bundleOutput -join "`n"
    if (($bundleExitCode -ne 0) -or ($bundleText -notmatch 'SOLUTION_BUNDLE='))
    {
        throw "Solution SmartBundle regeneration failed.`n$bundleText"
    }
    if (-not (Test-Path -LiteralPath $layout.SolutionBundle -PathType Leaf))
    {
        throw "Solution SmartBundle was not generated: $($layout.SolutionBundle)"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $cpu0SeedBundle) `
        -Force | Out-Null
    Copy-Item -LiteralPath $layout.SolutionBundle -Destination $cpu0SeedBundle
}

$linkerMetadata = @(
    (Join-Path $cpu0 'Debug\memory_regions.ld'),
    (Join-Path $cpu1 'Debug\memory_regions.ld')
)
if (@($linkerMetadata | Where-Object {
            -not (Test-Path -LiteralPath $_ -PathType Leaf)
        }).Count -ne 0)
{
    # The official Solution audit runs before DDSC content generation.  A
    # first clean build therefore needs one dependency-ordered DDSC pass to
    # create the child linker metadata that the audit validates.
    $metadataWorkspace = ('{0}-metadata-{1}' -f
        $Workspace, [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $metadataWorkspace -Force | Out-Null

    foreach ($project in @($solution, $cpu0, $cpu1))
    {
        [xml] $projectXml = Get-Content -Raw -LiteralPath (
            Join-Path $project '.project')
        $projectName = [string] $projectXml.projectDescription.name
        $savedErrorAction = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $importOutput = @(
            & $e2Cli -data $metadataWorkspace project import $project 2>&1
        )
        $importExitCode = $LASTEXITCODE
        $ErrorActionPreference = $savedErrorAction
        $importText = $importOutput -join "`n"
        $workspaceProject = Join-Path $metadataWorkspace (
            '.metadata\plugins\org.eclipse.core.resources\.projects\' +
            $projectName)
        if (($importText -match 'Failed to import project') -or
            (($importExitCode -ne 0) -and
             (-not (Test-Path -LiteralPath $workspaceProject -PathType Container))))
        {
            throw "Linker metadata bootstrap import failed: $project`n$importText"
        }
        if ($importExitCode -ne 0)
        {
            Write-Warning "e2 studio registered $projectName despite import exit code $importExitCode."
        }
    }

    foreach ($project in @($cpu0, $cpu1))
    {
        [xml] $projectXml = Get-Content -Raw -LiteralPath (
            Join-Path $project '.project')
        $projectName = [string] $projectXml.projectDescription.name
        $savedErrorAction = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $buildOutput = @(
            & $e2Cli -data $metadataWorkspace project build "$projectName/Debug" 2>&1
        )
        $buildExitCode = $LASTEXITCODE
        $ErrorActionPreference = $savedErrorAction
        $buildText = $buildOutput -join "`n"
        if (($buildExitCode -ne 0) -or
            ($buildText -notmatch 'Build Finished\. 0 errors') -or
            ($buildText -match 'Build encountered errors|Failed to build project'))
        {
            throw "Linker metadata bootstrap build failed: $projectName/Debug`n$buildText"
        }
    }

    foreach ($path in $linkerMetadata)
    {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf))
        {
            throw "DDSC bootstrap did not generate linker metadata: $path"
        }
    }
}

$cpu0Elf = $layout.Cpu0Elf
$cpu1Elf = $layout.Cpu1Elf
$sharedBefore = Get-Ra8p1SharedAbiSnapshot -SharedDirectory $shared
$sharedAfter = $null
$crossCoreAbiVerified = $false
$hadE2Override = Test-Path Env:RA8P1_E2_ROOT
$previousE2Override = $env:RA8P1_E2_ROOT

try
{
    $env:RA8P1_E2_ROOT = (Resolve-Path -LiteralPath $E2Root).Path
    & $dualBuild -Solution $solution -Cpu0Project $cpu0 -Cpu1Project $cpu1 `
        -Configuration Debug -Workspace $Workspace | Out-Null
    if (-not $?) { throw 'Official dual-core DDSC build failed.' }

    if (-not (Test-Path -LiteralPath $cpu0Elf -PathType Leaf)) { throw 'CPU0 ELF is missing.' }
    if (-not (Test-Path -LiteralPath $cpu1Elf -PathType Leaf)) { throw 'CPU1 ELF is missing.' }

    $abiOutput = @(& $crossCoreAbiVerifier -Cpu0Elf $cpu0Elf -Cpu1Elf $cpu1Elf -E2Root $E2Root)
    if (-not $?) { throw 'CPU0/CPU1 DWARF ABI verification failed.' }
    $crossCoreAbiVerified = $true
}
finally
{
    if ($hadE2Override)
    {
        $env:RA8P1_E2_ROOT = $previousE2Override
    }
    else
    {
        Remove-Item Env:RA8P1_E2_ROOT -ErrorAction SilentlyContinue
    }
    $sharedAfter = Get-Ra8p1SharedAbiSnapshot -SharedDirectory $shared
    Assert-Ra8p1SharedAbiUnchanged -Before $sharedBefore -After $sharedAfter -Stage 'dual-core build'
}

[pscustomobject]@{
    CPU0Elf = $cpu0Elf
    CPU0Bytes = (Get-Item -LiteralPath $cpu0Elf).Length
    CPU0Sha256 = (Get-FileHash -LiteralPath $cpu0Elf -Algorithm SHA256).Hash.ToUpperInvariant()
    CPU1Elf = $cpu1Elf
    CPU1Bytes = (Get-Item -LiteralPath $cpu1Elf).Length
    CPU1Sha256 = (Get-FileHash -LiteralPath $cpu1Elf -Algorithm SHA256).Hash.ToUpperInvariant()
    Workspace = $Workspace
    SharedAbiHeaders = $sharedAfter.HeaderCount
    SharedAbiSha256 = $sharedAfter.Digest
    CrossCoreAbiVerified = $crossCoreAbiVerified
    Flashed = $false
}

Set-StrictMode -Version Latest

function Resolve-Ra8p1ChildProject
{
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [string] $Solution,
        [Parameter(Mandatory = $true)]
        [ValidateSet('CPU0', 'CPU1')]
        [string] $Core
    )

    $solutionPath = (Resolve-Path -LiteralPath $Solution).Path
    $candidateNames = if ($Core -eq 'CPU0')
    {
        @('cpu0', 'ra8p1_sdr_stft_npu_display_solution_20260719_CPU0')
    }
    else
    {
        @('cpu1', 'ra8p1_sdr_stft_npu_display_solution_20260719_CPU1')
    }

    $matches = @($candidateNames | ForEach-Object {
        $candidate = Join-Path $solutionPath $_
        if (Test-Path -LiteralPath $candidate -PathType Container)
        {
            (Resolve-Path -LiteralPath $candidate).Path
        }
    })

    if ($matches.Count -eq 0)
    {
        $expected = @($candidateNames | ForEach-Object { Join-Path $solutionPath $_ }) -join ', '
        throw "$Core project directory was not found; tried: $expected"
    }
    if ($matches.Count -ne 1)
    {
        throw "$Core project layout is ambiguous; found: $($matches -join ', ')"
    }

    $directory = $matches[0]
    $projectFile = Join-Path $directory '.project'
    $configurationFile = Join-Path $directory 'configuration.xml'
    foreach ($required in @($projectFile, $configurationFile))
    {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf))
        {
            throw "$Core project input is missing: $required"
        }
    }

    [xml] $projectXml = Get-Content -Raw -LiteralPath $projectFile
    $projectName = [string] $projectXml.projectDescription.name
    if ([string]::IsNullOrWhiteSpace($projectName) -or
        $projectName -notmatch ("_{0}$" -f $Core))
    {
        throw "$Core .project has an invalid internal project name: $projectName"
    }

    [pscustomobject]@{
        Core = $Core
        Directory = $directory
        ProjectName = $projectName
        ProjectFile = $projectFile
        ConfigurationFile = $configurationFile
    }
}

function Resolve-Ra8p1ProjectLayout
{
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)] [string] $Solution)

    $solutionPath = (Resolve-Path -LiteralPath $Solution).Path
    foreach ($requiredName in @('.project', 'solution.xml'))
    {
        $required = Join-Path $solutionPath $requiredName
        if (-not (Test-Path -LiteralPath $required -PathType Leaf))
        {
            throw "Solution input is missing: $required"
        }
    }

    [xml] $solutionProjectXml = Get-Content -Raw -LiteralPath `
        (Join-Path $solutionPath '.project')
    $solutionProjectName = [string] $solutionProjectXml.projectDescription.name
    if ([string]::IsNullOrWhiteSpace($solutionProjectName))
    {
        throw 'Solution .project has no internal project name.'
    }

    $cpu0 = Resolve-Ra8p1ChildProject -Solution $solutionPath -Core CPU0
    $cpu1 = Resolve-Ra8p1ChildProject -Solution $solutionPath -Core CPU1
    if ($cpu0.Directory.Equals($cpu1.Directory, [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'CPU0 and CPU1 resolved to the same directory.'
    }

    $solutionText = Get-Content -Raw -LiteralPath (Join-Path $solutionPath 'solution.xml')
    foreach ($project in @($cpu0, $cpu1))
    {
        $bundleReference = '${workspace_loc:/' + $project.ProjectName + '}/'
        if ($solutionText.IndexOf($bundleReference, [StringComparison]::Ordinal) -lt 0)
        {
            throw "solution.xml does not reference $($project.ProjectName)."
        }
    }

    [pscustomobject]@{
        Solution = $solutionPath
        SolutionProjectName = $solutionProjectName
        SolutionBundle = Join-Path $solutionPath ("build\{0}.sbd" -f $solutionProjectName)
        Cpu0Directory = $cpu0.Directory
        Cpu0ProjectName = $cpu0.ProjectName
        Cpu1Directory = $cpu1.Directory
        Cpu1ProjectName = $cpu1.ProjectName
        Cpu0Elf = Join-Path $cpu0.Directory 'Debug\rtthread.elf'
        Cpu0Map = Join-Path $cpu0.Directory 'Debug\rtthread.map'
        Cpu1Elf = Join-Path $cpu1.Directory ("Debug\{0}.elf" -f $cpu1.ProjectName)
        Cpu1Map = Join-Path $cpu1.Directory ("Debug\{0}.map" -f $cpu1.ProjectName)
    }
}

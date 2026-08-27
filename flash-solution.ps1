[CmdletBinding()]
param(
    [string] $Workspace,
    [ValidatePattern('^[0-9]{6,20}$')] [string] $ProbeSerial = '1082495494',
    [string] $JLinkExe,
    [switch] $PreflightOnly,
    [switch] $Run
)

$ErrorActionPreference = 'Stop'
$ProbeSerial = $ProbeSerial.TrimStart('0')
$solution = Split-Path -Parent $MyInvocation.MyCommand.Path
$layoutHelper = Join-Path $solution 'tools\project-layout.ps1'
if (-not (Test-Path -LiteralPath $layoutHelper -PathType Leaf))
{
    throw "Project layout helper not found: $layoutHelper"
}
. $layoutHelper
$layout = Resolve-Ra8p1ProjectLayout -Solution $solution
if (-not $Workspace)
{
    $Workspace = Join-Path ([IO.Path]::GetTempPath()) `
        ('ra8p1-e2-flash-multicore-{0}' -f [Guid]::NewGuid().ToString('N'))
}
$Workspace = [IO.Path]::GetFullPath($Workspace)
$solutionPrefix = ([IO.Path]::GetFullPath($solution)).TrimEnd('\') + '\'
if ($Workspace.StartsWith($solutionPrefix, [StringComparison]::OrdinalIgnoreCase))
{
    throw "The e2 studio workspace must be outside the Solution directory: $Workspace"
}
$cpu0 = $layout.Cpu0Directory
$cpu1 = $layout.Cpu1Directory
$skillScripts = Join-Path $HOME '.codex\skills\ra8p1\scripts'
$launch = $layout.Cpu1ProjectName + ' Debug_Multicore'
$launchFile = Join-Path $cpu1 ($launch + '.launch')
if (-not (Test-Path -LiteralPath $launchFile -PathType Leaf))
{
    throw "Canonical CPU1 Debug_Multicore launch was not found: $launchFile"
}
$jlinkSettings = Join-Path $cpu0 ($launch + '.jlink')
$cpu1GdbCommands = Join-Path $cpu1 'Debug\gdbcmds.txt'
$cpu0Elf = $layout.Cpu0Elf
$cpu1Elf = $layout.Cpu1Elf

foreach ($path in @($cpu0, $cpu1, $cpu0Elf, $cpu1Elf, $skillScripts,
                    $launchFile, $jlinkSettings, $cpu1GdbCommands))
{
    if (-not (Test-Path -LiteralPath $path))
    {
        throw "Required flash input was not found: $path"
    }
}

if (-not $JLinkExe)
{
    $candidates = @()
    if ($env:RA8P1_JLINK_ROOT)
    {
        $candidates += Join-Path $env:RA8P1_JLINK_ROOT 'JLink.exe'
    }
    $hostConfig = Join-Path $HOME '.codex\ra8p1.json'
    if (Test-Path -LiteralPath $hostConfig -PathType Leaf)
    {
        $config = Get-Content -Raw -LiteralPath $hostConfig | ConvertFrom-Json
        if ($config.JLinkRoot)
        {
            $candidates += Join-Path ([string] $config.JLinkRoot) 'JLink.exe'
        }
    }
    $candidates += 'C:\Program Files\SEGGER\JLink_V956\JLink.exe'
    $candidates += 'C:\Program Files\SEGGER\JLink\JLink.exe'
    $JLinkExe = $candidates |
        Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } |
        Select-Object -First 1
}
if (-not $JLinkExe -or -not (Test-Path -LiteralPath $JLinkExe -PathType Leaf))
{
    throw 'JLink.exe was not found. Pass -JLinkExe or set RA8P1_JLINK_ROOT.'
}
$JLinkExe = (Resolve-Path -LiteralPath $JLinkExe).Path

function Get-ConnectedJLinkSerials
{
    param([Parameter(Mandatory = $true)] [string] $Executable)

    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.Arguments = '-AutoConnect 0'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw 'Could not start J-Link Commander.' }
    try
    {
        $process.StandardInput.WriteLine('ShowEmuList')
        $process.StandardInput.WriteLine('exit')
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(15000))
        {
            $process.Kill()
            throw 'J-Link probe enumeration timed out.'
        }
        $text = $process.StandardOutput.ReadToEnd() + $process.StandardError.ReadToEnd()
    }
    finally
    {
        $process.Dispose()
    }
    $serials = @([regex]::Matches(
        $text, '(?im)(?:S/N:|Serial number:)\s*(?<serial>[0-9]+)') |
        ForEach-Object { $_.Groups['serial'].Value.TrimStart('0') } |
        Sort-Object -Unique)
    if ($serials.Count -eq 0)
    {
        throw "No J-Link probe was discovered.`n$text"
    }
    return $serials
}

$connectedSerials = @(Get-ConnectedJLinkSerials -Executable $JLinkExe)
if ($connectedSerials -notcontains $ProbeSerial)
{
    throw "Requested J-Link probe $ProbeSerial is absent; connected: $($connectedSerials -join ', ')."
}

[xml] $launchXml = Get-Content -Raw -LiteralPath $launchFile
function Get-LaunchString
{
    param([Parameter(Mandatory = $true)] [string] $Key)
    $node = $launchXml.launchConfiguration.stringAttribute |
        Where-Object { $_.key -eq $Key } |
        Select-Object -First 1
    if (-not $node) { throw "Launch is missing string attribute: $Key" }
    return [string] $node.value
}

$target = Get-LaunchString 'com.renesas.cdt.core.targetDevice'
$interface = Get-LaunchString 'com.renesas.hardwaredebug.arm.jlink.interface.type'
$launchSerial = (Get-LaunchString 'com.renesas.hardwaredebug.arm.jlink.jlink.usbSerial').TrimStart('0')
$projectName = Get-LaunchString 'org.eclipse.cdt.launch.PROJECT_ATTR'
$programName = Get-LaunchString 'org.eclipse.cdt.launch.PROGRAM_NAME'
$runCommands = Get-LaunchString 'org.eclipse.cdt.debug.gdbjtag.core.runCommands'
$speedNode = $launchXml.launchConfiguration.intAttribute |
    Where-Object { $_.key -eq 'com.renesas.hardwaredebug.arm.jlink.interface.speed' } |
    Select-Object -First 1
$downloadNode = $launchXml.launchConfiguration.listAttribute |
    Where-Object { $_.key -eq 'com.renesas.cdt.launch.dsf.downloadImages' } |
    Select-Object -First 1
$downloadImages = @($downloadNode.listEntry | ForEach-Object { [string] $_.value })

if ($target -ne 'R7KA8P1KF_CPU0' -or $interface -ne 'SWD' -or
    (-not $speedNode) -or ([int] $speedNode.value -ne 4000))
{
    throw 'Multicore launch is not pinned to R7KA8P1KF_CPU0/SWD/4000 kHz.'
}
if ($launchSerial -ne $ProbeSerial)
{
    throw "Multicore launch is pinned to probe $launchSerial, not requested probe $ProbeSerial."
}
if (($projectName -ne $layout.Cpu0ProjectName) -or
    ($programName -ne 'Debug/rtthread.elf'))
{
    throw 'Multicore launch primary image is not the CPU0 rtthread ELF.'
}
$expectedCpu1Download = ('${workspace_loc:/' + $layout.Cpu1ProjectName +
    '}/Debug/' + $layout.Cpu1ProjectName + '.elf|true|true|false|0|true|No core')
if ($downloadImages -notcontains $expectedCpu1Download)
{
    throw 'Multicore launch does not include the CPU1 ELF as a download image.'
}
$cpu1WorkspaceReference = '${workspace_loc:/' + $layout.Cpu1ProjectName + '}'
if (($runCommands -notmatch
     ('cd\s+' + [regex]::Escape($cpu1WorkspaceReference))) -or
    ($runCommands -notmatch 'source\s+Debug/gdbcmds\.txt'))
{
    throw 'Multicore launch does not enter the CPU1 directory and execute its release command file.'
}
$jlinkText = Get-Content -Raw -LiteralPath $jlinkSettings
if ($jlinkText -notmatch '(?m)^\s*VerifyDownload\s*=\s*1\s*$')
{
    throw 'J-Link settings do not enable VerifyDownload=1.'
}
$gdbCommandText = Get-Content -Raw -LiteralPath $cpu1GdbCommands
foreach ($required in @('0x4000F054', '0x4000F044', '0x2080000', '0x4000F064'))
{
    if ($gdbCommandText -notmatch [regex]::Escape($required))
    {
        throw "CPU1 release command file is missing $required."
    }
}

& (Join-Path $skillScripts 'ra8p1-solution-inspect.ps1') `
    -Solution $solution -Cpu0Project $cpu0 -Cpu1Project $cpu1 | Out-Null

if ($PreflightOnly)
{
    [pscustomobject]@{
        Status = 'preflight-pass'
        ProbeSerial = $ProbeSerial
        ConnectedProbeSerials = $connectedSerials
        Target = $target
        Interface = $interface
        SpeedKHz = [int] $speedNode.value
        Launch = $launch
        CPU0Elf = $cpu0Elf
        CPU0Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $cpu0Elf).Hash.ToUpperInvariant()
        CPU1Elf = $cpu1Elf
        CPU1Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $cpu1Elf).Hash.ToUpperInvariant()
        VerifyDownload = $true
        CPU1ReleaseCommands = $true
        Flashed = $false
    }
    return
}

$arguments = @{
    Project = $solution
    RelatedProject = @($cpu0, $cpu1)
    Launch = $launch
    Workspace = $Workspace
    SkipBuild = $true
    Background = $true
    PreRunCommand = @('delete breakpoints')
}
if ($Run)
{
    $arguments.Run = $true
    $arguments.PostRunDelaySeconds = 3
}

& (Join-Path $skillScripts 'ra8p1-e2-flash.ps1') @arguments

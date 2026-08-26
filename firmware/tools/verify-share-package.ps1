[CmdletBinding()]
param(
    [string] $Root
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($Root))
{
    $Root = Split-Path -Parent $PSScriptRoot
}
$Root = (Resolve-Path -LiteralPath $Root).Path
$manifest = Join-Path $Root 'MANIFEST.sha256'
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf))
{
    throw "MANIFEST.sha256 was not found under $Root"
}

foreach ($relative in @('tools\sdr_libdl_glibc_225.map',
                         'tools\sdr_libpthread_glibc_225.map'))
{
    $required = Join-Path $Root $relative
    if (-not (Test-Path -LiteralPath $required -PathType Leaf))
    {
        throw "Required SDR build input is missing: $relative"
    }
}
$layoutHelper = Join-Path $Root 'tools\project-layout.ps1'
. $layoutHelper
$layout = Resolve-Ra8p1ProjectLayout -Solution $Root
if (-not (Test-Path -LiteralPath $layout.SolutionBundle -PathType Leaf))
{
    throw "Required Solution SmartBundle is missing: $($layout.SolutionBundle)"
}

$checked = 0
foreach ($line in Get-Content -LiteralPath $manifest)
{
    if ([string]::IsNullOrWhiteSpace($line))
    {
        continue
    }

    if ($line -notmatch '^(?<hash>[0-9A-F]{64}) \*(?<relative>.+)$')
    {
        throw "Invalid manifest line: $line"
    }

    $relative = $matches.relative.Replace('/', '\')
    $path = Join-Path $Root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf))
    {
        throw "Manifest file is missing: $relative"
    }

    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actual -ne $matches.hash)
    {
        throw "SHA-256 mismatch: $relative`nexpected $($matches.hash)`nactual   $actual"
    }
    $checked++
}

[pscustomobject]@{
    Status = 'pass'
    Root = $Root
    FilesVerified = $checked
}

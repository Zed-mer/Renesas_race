[CmdletBinding()]
param(
    [string] $Elf = (Join-Path $PSScriptRoot '..\cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf'),
    [string] $Header = (Join-Path $PSScriptRoot '..\cpu1\src\display_bringup.h'),
    [ValidatePattern('^[0-9]{6,20}$')] [string] $ProbeSerial = '1082495494',
    [string] $JLinkExe = 'C:\Program Files\SEGGER\JLink_V956\JLink.exe',
    [string] $NmExe = 'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe',
    [string] $OutputPath,
    [switch] $Halt
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach ($path in @($Elf, $Header, $JLinkExe, $NmExe))
{
    if (-not (Test-Path -LiteralPath $path -PathType Leaf))
    {
        throw "Required display diagnostic input was not found: $path"
    }
}

$Elf = (Resolve-Path -LiteralPath $Elf).Path
$Header = (Resolve-Path -LiteralPath $Header).Path
$JLinkExe = (Resolve-Path -LiteralPath $JLinkExe).Path
$NmExe = (Resolve-Path -LiteralPath $NmExe).Path
$ProbeSerial = $ProbeSerial.TrimStart('0')

$symbolLine = @(& $NmExe -S -C $Elf) |
    Where-Object { $_ -match '\bg_display_diag$' } |
    Select-Object -First 1
if (-not $symbolLine -or
    $symbolLine -notmatch '^(?<address>[0-9A-Fa-f]+)\s+(?<size>[0-9A-Fa-f]+)\s+\S\s+g_display_diag$')
{
    throw 'g_display_diag was not found in the selected CPU1 ELF.'
}
$address = [Convert]::ToUInt32($Matches.address, 16)
$symbolSize = [Convert]::ToUInt32($Matches.size, 16)

$fields = [Collections.Generic.List[string]]::new()
$inside = $false
foreach ($line in Get-Content -LiteralPath $Header)
{
    if ($line -match '^typedef struct st_display_diag')
    {
        $inside = $true
        continue
    }
    if ($inside -and $line -match '^\s*}\s*display_diag_t')
    {
        break
    }
    if ($inside -and
        $line -match '^\s*(?:u?int32_t)\s+(?<name>[A-Za-z0-9_]+)(?:\[(?<count>[0-9]+)\])?\s*;')
    {
        $fieldName = $Matches['name']
        $arrayCountText = $Matches['count']
        if ($arrayCountText)
        {
            $fieldCount = [int]$arrayCountText
            for ($fieldIndex = 0; $fieldIndex -lt $fieldCount; ++$fieldIndex)
            {
                $fields.Add(('{0}[{1}]' -f $fieldName, $fieldIndex))
            }
        }
        else
        {
            $fields.Add($fieldName)
        }
    }
}
if (($fields.Count * 4) -ne $symbolSize)
{
    throw "Header/ELF display_diag size mismatch: fields=$($fields.Count * 4), ELF=$symbolSize."
}

$countHex = '0x{0:X}' -f $fields.Count
$addressHex = '0x{0:X8}' -f $address
$commands = [Collections.Generic.List[string]]::new()
if ($Halt)
{
    [void] $commands.Add('halt')
}
[void] $commands.Add("mem32 $addressHex, $countHex")
if ($Halt)
{
    [void] $commands.Add('go')
}
[void] $commands.Add('exit')
$arguments = @(
    '-NoGui', '1',
    '-AutoConnect', '1',
    '-Device', 'R7KA8P1KF_CPU0',
    '-If', 'SWD',
    '-Speed', '4000',
    '-SelectEmuBySN', $ProbeSerial
)
$rawLines = @($commands | & $JLinkExe @arguments 2>&1)
$exitCode = $LASTEXITCODE
$rawText = $rawLines -join "`n"
if (($exitCode -ne 0) -or
    ($rawText -notmatch "S/N:\s*$([regex]::Escape($ProbeSerial))") -or
    ($rawText -notmatch 'Device "R7KA8P1KF_CPU0" selected') -or
    ($rawText -notmatch 'Cortex-M85 identified') -or
    ($rawText -match '(?i)cannot connect|connection failed|error while|unknown command'))
{
    throw "J-Link display diagnostic read failed.`n$rawText"
}

$words = New-Object 'uint32[]' $fields.Count
$seen = New-Object 'bool[]' $fields.Count
foreach ($line in $rawLines)
{
    $text = [string] $line
    if ($text -notmatch '^(?:J-Link>)?(?<address>[0-9A-Fa-f]{8})\s*=\s*(?<values>.*)$')
    {
        continue
    }
    $lineAddress = [Convert]::ToUInt32($Matches.address, 16)
    if (($lineAddress -lt $address) -or ($lineAddress -ge ($address + $symbolSize)))
    {
        continue
    }
    $tokens = @($Matches.values -split '\s+' |
        Where-Object { $_ -match '^[0-9A-Fa-f]{8}$' })
    $startIndex = [int](($lineAddress - $address) / 4)
    for ($offset = 0; $offset -lt $tokens.Count; ++$offset)
    {
        $index = $startIndex + $offset
        if ($index -ge $fields.Count)
        {
            break
        }
        $words[$index] = [Convert]::ToUInt32($tokens[$offset], 16)
        $seen[$index] = $true
    }
}
if ($seen -contains $false)
{
    $missing = for ($index = 0; $index -lt $seen.Count; ++$index)
    {
        if (-not $seen[$index]) { $fields[$index] }
    }
    throw "J-Link did not return the complete display_diag object: $($missing -join ', ')."
}

$diag = [ordered]@{}
for ($index = 0; $index -lt $fields.Count; ++$index)
{
    $diag[$fields[$index]] = [uint32] $words[$index]
}

$result = [ordered]@{
    CapturedAt = (Get-Date).ToString('o')
    ProbeSerial = $ProbeSerial
    Target = 'R7KA8P1KF_CPU0'
    CPU1Elf = $Elf
    CPU1ElfSha256 = (Get-FileHash -LiteralPath $Elf -Algorithm SHA256).Hash.ToUpperInvariant()
    SymbolAddress = $addressHex
    SymbolSize = $symbolSize
    AccessMode = if ($Halt) { 'halted' } else { 'live-memory' }
    Snapshot = $diag
}
$json = $result | ConvertTo-Json -Depth 4
if ($OutputPath)
{
    $parent = Split-Path -Parent $OutputPath
    if ($parent)
    {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Set-Content -LiteralPath $OutputPath -Value $json -Encoding utf8
}
$json

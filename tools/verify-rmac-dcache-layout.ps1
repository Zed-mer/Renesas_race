[CmdletBinding()]
param(
    [string] $Project,
    [string] $Map,
    [UInt64] $SharedRamStart = 0x220E2000,
    [switch] $SelfTest
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = "Stop"

function Convert-HexToUInt64
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Value
    )

    if ($Value -notmatch '^0x[0-9A-Fa-f]+$')
    {
        throw "Invalid hexadecimal value '$Value'."
    }

    return [Convert]::ToUInt64($Value.Substring(2), 16)
}

function Format-HexAddress
{
    param(
        [Parameter(Mandatory = $true)]
        [UInt64] $Value
    )

    return ('0x{0:X8}' -f $Value)
}

function Get-UniqueMapSymbol
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $MapText,

        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $escapedName = [Regex]::Escape($Name)
    $matches = [Regex]::Matches(
        $MapText,
        "(?m)^[ \t]*(?<address>0x[0-9A-Fa-f]+)[ \t]+$escapedName[ \t]*=")

    if ($matches.Count -ne 1)
    {
        throw "Expected exactly one definition of map symbol '$Name', found $($matches.Count)."
    }

    return [PSCustomObject]@{
        Name    = $Name
        Address = Convert-HexToUInt64 $matches[0].Groups['address'].Value
        Index   = $matches[0].Index
    }
}

function Get-RmacTargetRecords
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $MapText
    )

    $symbolPattern = 'g_ether0_(?:ether_buffer[0-9]+|(?:ts|tx|rx)_descriptor_array[0-9]+)'
    $recordPattern = "(?m)^[ \t]*(?<section>\.bss\.(?<symbol>$symbolPattern))(?:[ \t]+(?<address>0x[0-9A-Fa-f]+)[ \t]+(?<size>0x[0-9A-Fa-f]+)[ \t]+(?<object>[^\r\n]+)|[ \t]*\r?\n[ \t]+(?<address>0x[0-9A-Fa-f]+)[ \t]+(?<size>0x[0-9A-Fa-f]+)[ \t]+(?<object>[^\r\n]+))[ \t]*\r?$"
    $matches = [Regex]::Matches($MapText, $recordPattern)
    $records = @()

    foreach ($match in $matches)
    {
        $records += [PSCustomObject]@{
            Section = $match.Groups['section'].Value
            Symbol  = $match.Groups['symbol'].Value
            Address = Convert-HexToUInt64 $match.Groups['address'].Value
            Size    = Convert-HexToUInt64 $match.Groups['size'].Value
            Object  = $match.Groups['object'].Value.Trim()
            Index   = $match.Index
        }
    }

    return $records
}

function Get-IqRingMapRecord
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $MapText
    )

    $recordPattern = '(?m)^[ \t]*(?<section>\.sdram_noinit)(?:[ \t]+(?<address>0x[0-9A-Fa-f]+)[ \t]+(?<size>0x[0-9A-Fa-f]+)[ \t]+(?<object>[^\r\n]+)|[ \t]*\r?\n[ \t]+(?<address>0x[0-9A-Fa-f]+)[ \t]+(?<size>0x[0-9A-Fa-f]+)[ \t]+(?<object>[^\r\n]+))[ \t]*\r?$'
    $matches = @([Regex]::Matches($MapText, $recordPattern) | Where-Object {
        $_.Groups['object'].Value.Replace('\', '/') -match '(?:^|/)src/framework/iq_ring\.o(?:\s|$)'
    })

    if ($matches.Count -ne 1)
    {
        throw "Expected exactly one .sdram_noinit input record from src/framework/iq_ring.o, found $($matches.Count)."
    }

    return [PSCustomObject]@{
        Section = $matches[0].Groups['section'].Value
        Address = Convert-HexToUInt64 $matches[0].Groups['address'].Value
        Size    = Convert-HexToUInt64 $matches[0].Groups['size'].Value
        Object  = $matches[0].Groups['object'].Value.Trim()
        Index   = $matches[0].Index
    }
}

function Test-RmacDcacheLayout
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $HeaderText,

        [Parameter(Mandatory = $true)]
        [string] $MapText,

        [Parameter(Mandatory = $true)]
        [string] $IqRingSourceText,

        [Parameter(Mandatory = $true)]
        [UInt64] $SharedStart
    )

    $dcachePattern = '(?ms)^[ \t]*#[ \t]*if[ \t]*\([ \t]*0U[ \t]*==[ \t]*BSP_CFG_CPU_CORE[ \t]*\)[ \t]*\r?\n[ \t]*#[ \t]*define[ \t]+BSP_CFG_DCACHE_ENABLED[ \t]+\([ \t]*1U?[ \t]*\)'
    if (-not [Regex]::IsMatch($HeaderText, $dcachePattern))
    {
        throw 'Generated CPU0 configuration does not define BSP_CFG_DCACHE_ENABLED as 1.'
    }

    $rmacBase = Get-UniqueMapSymbol $MapText '__rmac_dma_nocache$$Base'
    $rmacLimit = Get-UniqueMapSymbol $MapText '__rmac_dma_nocache$$Limit'
    $ramZeroBase = Get-UniqueMapSymbol $MapText '__ram_zero$$Base'
    $ramZeroLimit = Get-UniqueMapSymbol $MapText '__ram_zero$$Limit'
    $sdramNoInitBase = Get-UniqueMapSymbol $MapText '__sdram_noinit$$Base'
    $sdramNoInitLimit = Get-UniqueMapSymbol $MapText '__sdram_noinit$$Limit'

    if (($rmacBase.Address -ge $rmacLimit.Address) -or ($rmacBase.Index -ge $rmacLimit.Index))
    {
        throw 'RMAC non-cache Base/Limit are empty, reversed, or out of map order.'
    }
    if ((($rmacBase.Address % 32) -ne 0) -or (($rmacLimit.Address % 32) -ne 0))
    {
        throw 'RMAC non-cache Base/Limit must both be 32-byte aligned.'
    }
    if (($ramZeroBase.Address -gt $ramZeroLimit.Address) -or ($ramZeroBase.Index -ge $ramZeroLimit.Index))
    {
        throw 'Ordinary RAM zero Base/Limit are reversed or out of map order.'
    }
    if (($sdramNoInitBase.Address -ge $sdramNoInitLimit.Address) -or ($sdramNoInitBase.Index -ge $sdramNoInitLimit.Index))
    {
        throw 'SDRAM no-init Base/Limit are empty, reversed, or out of map order.'
    }

    $expectedBuffers = @(0..191 | ForEach-Object { "g_ether0_ether_buffer$_" })
    $expectedDescriptors = @(
        'g_ether0_ts_descriptor_array0',
        'g_ether0_tx_descriptor_array0',
        'g_ether0_tx_descriptor_array1',
        'g_ether0_rx_descriptor_array0',
        'g_ether0_rx_descriptor_array1'
    )
    $expectedSymbols = @($expectedBuffers + $expectedDescriptors)
    $records = @(Get-RmacTargetRecords $MapText)
    $actualSymbols = @($records | ForEach-Object { $_.Symbol })

    $duplicates = @($actualSymbols | Group-Object | Where-Object { $_.Count -ne 1 } | ForEach-Object { $_.Name })
    $missing = @($expectedSymbols | Where-Object { $actualSymbols -notcontains $_ })
    $unexpected = @($actualSymbols | Where-Object { $expectedSymbols -notcontains $_ } | Select-Object -Unique)

    if (($records.Count -ne 197) -or ($duplicates.Count -ne 0) -or ($missing.Count -ne 0) -or ($unexpected.Count -ne 0))
    {
        throw "RMAC DMA object set mismatch: records=$($records.Count), duplicates=$($duplicates -join ','), missing=$($missing -join ','), unexpected=$($unexpected -join ',')."
    }

    foreach ($record in $records)
    {
        $normalizedObject = $record.Object.Replace('\', '/')
        if ($normalizedObject -notmatch '(?:^|/)ra_gen/hal_data\.o(?:\s|$)')
        {
            throw "RMAC DMA section '$($record.Section)' comes from unexpected object '$($record.Object)'."
        }

        if ($record.Size -eq 0)
        {
            throw "RMAC DMA section '$($record.Section)' has zero size."
        }

        $endAddress = $record.Address + $record.Size
        if (($endAddress -lt $record.Address) -or
            ($record.Address -lt $rmacBase.Address) -or
            ($endAddress -gt $rmacLimit.Address))
        {
            throw "RMAC DMA section '$($record.Section)' at $(Format-HexAddress $record.Address) size $(Format-HexAddress $record.Size) is outside __rmac_dma_nocache bounds."
        }

        if (($record.Index -le $rmacBase.Index) -or ($record.Index -ge $rmacLimit.Index))
        {
            throw "RMAC DMA section '$($record.Section)' is not emitted inside the marked __rmac_dma_nocache map block."
        }

        if (($record.Index -gt $ramZeroBase.Index) -and ($record.Index -lt $ramZeroLimit.Index))
        {
            throw "RMAC DMA section '$($record.Section)' also appears in the ordinary .bss/__ram_zero block."
        }
    }

    $ordinaryBssText = $MapText.Substring(
        $ramZeroBase.Index,
        $ramZeroLimit.Index + $ramZeroLimit.Name.Length - $ramZeroBase.Index)
    foreach ($symbol in $expectedSymbols)
    {
        $symbolPattern = '(?m)(?<![A-Za-z0-9_])' + [Regex]::Escape($symbol) + '(?![A-Za-z0-9_])'
        if ([Regex]::IsMatch($ordinaryBssText, $symbolPattern))
        {
            throw "Ordinary .bss/__ram_zero block contains a copy of '$symbol'."
        }
    }

    $iqSourcePattern = '(?s)\bg_iq_ring\s*\[[^\]]+\]\s*__attribute__\s*\(\s*\(\s*section\s*\(\s*"\.sdram_noinit"\s*\)'
    if (-not [Regex]::IsMatch($IqRingSourceText, $iqSourcePattern))
    {
        throw 'g_iq_ring source declaration is not explicitly assigned to .sdram_noinit.'
    }

    if ([Regex]::IsMatch($MapText, '(?m)^[ \t]*\.bss\.g_iq_ring(?:[ \t]|$)'))
    {
        throw 'Map contains a normal .bss.g_iq_ring input section.'
    }

    $iqRingRecord = Get-IqRingMapRecord $MapText
    $iqRingEnd = $iqRingRecord.Address + $iqRingRecord.Size
    if (($iqRingRecord.Size -eq 0) -or
        ($iqRingEnd -lt $iqRingRecord.Address) -or
        ($iqRingRecord.Address -lt $sdramNoInitBase.Address) -or
        ($iqRingEnd -gt $sdramNoInitLimit.Address) -or
        ($iqRingRecord.Index -le $sdramNoInitBase.Index) -or
        ($iqRingRecord.Index -ge $sdramNoInitLimit.Index))
    {
        throw 'iq_ring.o .sdram_noinit storage is outside the __sdram_noinit Base/Limit block.'
    }

    if ($ramZeroLimit.Address -ge $SharedStart)
    {
        throw "Internal RAM use ends at $(Format-HexAddress $ramZeroLimit.Address), which is not below shared RAM start $(Format-HexAddress $SharedStart)."
    }

    return [PSCustomObject]@{
        DcacheEnabled  = 1
        BufferCount    = $expectedBuffers.Count
        DescriptorCount = $expectedDescriptors.Count
        RmacBase       = $rmacBase.Address
        RmacLimit      = $rmacLimit.Address
        IqRingAddress  = $iqRingRecord.Address
        IqRingSize     = $iqRingRecord.Size
        RamEnd         = $ramZeroLimit.Address
        SharedRamStart = $SharedStart
    }
}

function New-SelfTestMap
{
    param(
        [int] $MissingBuffer = -1,
        [switch] $DescriptorOutOfBounds,
        [switch] $OrdinaryBssCopy,
        [switch] $WrongIqRingSection,
        [UInt64] $RamEnd = 0x22003000
    )

    $builder = New-Object System.Text.StringBuilder
    [void] $builder.AppendLine('__ram_zero_nocache$$')
    [void] $builder.AppendLine('                0x22000000                __ram_zero_nocache$$Base = .')
    [void] $builder.AppendLine('                0x22000100                __rmac_dma_nocache$$Base = .')
    $nextAddress = [UInt64] 0x22000100

    foreach ($index in 0..191)
    {
        if ($index -eq $MissingBuffer)
        {
            continue
        }
        [void] $builder.AppendLine(" .bss.g_ether0_ether_buffer$index")
        [void] $builder.AppendLine(('                0x{0:X8}        0x4 ./ra_gen/hal_data.o' -f $nextAddress))
        $nextAddress += 0x10
    }

    $descriptors = @(
        'g_ether0_ts_descriptor_array0',
        'g_ether0_tx_descriptor_array0',
        'g_ether0_tx_descriptor_array1',
        'g_ether0_rx_descriptor_array0',
        'g_ether0_rx_descriptor_array1'
    )
    foreach ($descriptor in $descriptors)
    {
        $descriptorAddress = $nextAddress
        if ($DescriptorOutOfBounds -and ($descriptor -eq 'g_ether0_rx_descriptor_array1'))
        {
            $descriptorAddress = 0x22001000
        }
        [void] $builder.AppendLine(" .bss.$descriptor")
        [void] $builder.AppendLine(('                0x{0:X8}       0x10 ./ra_gen/hal_data.o' -f $descriptorAddress))
        $nextAddress += 0x10
    }

    [void] $builder.AppendLine('                0x22001000                __rmac_dma_nocache$$Limit = .')
    [void] $builder.AppendLine('                0x22001000                __ram_zero_nocache$$Limit = .')
    [void] $builder.AppendLine('__ram_zero$$')
    [void] $builder.AppendLine('                0x22002000                __ram_zero$$Base = .')
    if ($OrdinaryBssCopy)
    {
        [void] $builder.AppendLine(' .bss.g_ether0_ether_buffer0')
        [void] $builder.AppendLine('                0x22002000        0x4 ./ra_gen/hal_data.o')
    }
    [void] $builder.AppendLine(('                0x{0:X8}                __ram_zero$$Limit = .' -f $RamEnd))
    [void] $builder.AppendLine('__sdram_noinit$$')
    [void] $builder.AppendLine('                0x68000000                __sdram_noinit$$Base = .')
    if ($WrongIqRingSection)
    {
        [void] $builder.AppendLine(' .bss.g_iq_ring 0x68000100 0x620000 ./src/framework/iq_ring.o')
    }
    else
    {
        [void] $builder.AppendLine(' .sdram_noinit  0x68000100 0x620000 ./src/framework/iq_ring.o')
    }
    [void] $builder.AppendLine('                0x68620100                __sdram_noinit$$Limit = .')
    return $builder.ToString()
}

function Assert-SelfTestFailure
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name,

        [Parameter(Mandatory = $true)]
        [scriptblock] $Action
    )

    try
    {
        & $Action | Out-Null
    }
    catch
    {
        return
    }

    throw "Self-test '$Name' unexpectedly passed."
}

function Invoke-SelfTest
{
    $validHeader = @'
#ifndef BSP_CFG_DCACHE_ENABLED
 #if (0U == BSP_CFG_CPU_CORE)
    #define BSP_CFG_DCACHE_ENABLED (1)
 #else
    #define BSP_CFG_DCACHE_ENABLED (0)
 #endif
#endif
'@
    $invalidHeader = $validHeader.Replace('BSP_CFG_DCACHE_ENABLED (1)', 'BSP_CFG_DCACHE_ENABLED (0)')
    $iqRingSource = @'
static iq_ring_slot_t g_iq_ring[IQ_RING_SLOT_COUNT]
    __attribute__((section(".sdram_noinit"), aligned(32), used));
'@
    $validMap = New-SelfTestMap

    $result = Test-RmacDcacheLayout $validHeader $validMap $iqRingSource 0x220E2000
    if (($result.BufferCount -ne 192) -or ($result.DescriptorCount -ne 5))
    {
        throw 'Valid fixture returned unexpected RMAC object counts.'
    }

    Assert-SelfTestFailure 'dcache-disabled' {
        Test-RmacDcacheLayout $invalidHeader $validMap $iqRingSource 0x220E2000
    }
    Assert-SelfTestFailure 'missing-buffer' {
        Test-RmacDcacheLayout $validHeader (New-SelfTestMap -MissingBuffer 191) $iqRingSource 0x220E2000
    }
    Assert-SelfTestFailure 'descriptor-out-of-bounds' {
        Test-RmacDcacheLayout $validHeader (New-SelfTestMap -DescriptorOutOfBounds) $iqRingSource 0x220E2000
    }
    Assert-SelfTestFailure 'ordinary-bss-copy' {
        Test-RmacDcacheLayout $validHeader (New-SelfTestMap -OrdinaryBssCopy) $iqRingSource 0x220E2000
    }
    Assert-SelfTestFailure 'wrong-iq-ring-section' {
        Test-RmacDcacheLayout $validHeader (New-SelfTestMap -WrongIqRingSection) $iqRingSource 0x220E2000
    }
    Assert-SelfTestFailure 'ram-collision' {
        Test-RmacDcacheLayout $validHeader (New-SelfTestMap -RamEnd 0x220E2000) $iqRingSource 0x220E2000
    }

    Write-Output 'RMAC_DCACHE_LAYOUT SELFTEST PASS cases=7'
}

try
{
    if ($SelfTest)
    {
        Invoke-SelfTest
        exit 0
    }

    if ([string]::IsNullOrWhiteSpace($Project))
    {
        $layoutHelper = Join-Path $PSScriptRoot 'project-layout.ps1'
        . $layoutHelper
        $Project = (Resolve-Ra8p1ProjectLayout `
            -Solution (Split-Path -Parent $PSScriptRoot)).Cpu0Directory
    }
    if (-not (Test-Path -LiteralPath $Project -PathType Container))
    {
        throw "CPU0 project directory not found: $Project"
    }
    $projectPath = (Resolve-Path -LiteralPath $Project).Path

    if ([string]::IsNullOrWhiteSpace($Map))
    {
        $Map = Join-Path $projectPath 'Debug\rtthread.map'
    }
    if (-not (Test-Path -LiteralPath $Map -PathType Leaf))
    {
        throw "Linker map not found: $Map"
    }

    $headerPath = Join-Path $projectPath 'ra_cfg\fsp_cfg\bsp\bsp_mcu_family_cfg.h'
    $iqRingSourcePath = Join-Path $projectPath 'src\framework\iq_ring.c'
    foreach ($requiredPath in @($headerPath, $iqRingSourcePath))
    {
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf))
        {
            throw "Required input not found: $requiredPath"
        }
    }

    $headerText = [IO.File]::ReadAllText($headerPath)
    $mapText = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $Map).Path)
    $iqRingSourceText = [IO.File]::ReadAllText($iqRingSourcePath)
    $result = Test-RmacDcacheLayout $headerText $mapText $iqRingSourceText $SharedRamStart

    Write-Output (
        'RMAC_DCACHE_LAYOUT PASS dcache={0} buffers={1} descriptors={2} rmac_base={3} rmac_limit={4} iq_ring={5} iq_ring_size={6} ram_end={7} shared_start={8}' -f
        $result.DcacheEnabled,
        $result.BufferCount,
        $result.DescriptorCount,
        (Format-HexAddress $result.RmacBase),
        (Format-HexAddress $result.RmacLimit),
        (Format-HexAddress $result.IqRingAddress),
        (Format-HexAddress $result.IqRingSize),
        (Format-HexAddress $result.RamEnd),
        (Format-HexAddress $result.SharedRamStart))
}
catch
{
    Write-Error ("RMAC_DCACHE_LAYOUT FAIL: {0}" -f $_.Exception.Message)
    exit 1
}

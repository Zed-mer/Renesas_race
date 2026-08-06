[CmdletBinding()]
param(
    [string] $ProbeSerial,
    [string] $JLinkExe,
    [string] $Cpu0Elf,
    [string] $Cpu1Elf,
    [ValidateRange(0, 3600)] [int] $WindowSeconds = 0,
    [switch] $Json,
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$layoutHelper = Join-Path $PSScriptRoot 'project-layout.ps1'
if (-not (Test-Path -LiteralPath $layoutHelper -PathType Leaf)) {
    throw "Project layout helper not found: $layoutHelper"
}
. $layoutHelper
$script:ProjectLayout = Resolve-Ra8p1ProjectLayout -Solution $script:ProjectRoot
$script:Layout = $null
$script:HeaderValues = $null

function Format-Address {
    param([uint32] $Address)
    return ('0x{0:X8}' -f $Address)
}

function Add-Address {
    param([uint32] $Base, [int] $Offset)
    return [uint32] ([uint64] $Base + [uint64] $Offset)
}

function Get-IntegerMacro {
    param(
        [Parameter(Mandatory)] [string] $Text,
        [Parameter(Mandatory)] [string] $Name,
        [string[]] $Stack = @()
    )

    $escaped = [regex]::Escape($Name)
    if ($Stack -contains $Name) {
        throw "Circular numeric macro definition: $($Stack -join ' -> ') -> $Name."
    }
    $pattern = "(?m)^\s*#define\s+$escaped[ \t]+(?<definition>(?:[^\r\n]*\\\r?\n)*[^\r\n]*)"
    $match = [regex]::Match($Text, $pattern)
    if (-not $match.Success) {
        throw "Could not read numeric macro $Name."
    }
    $definition = $match.Groups['definition'].Value
    $definition = $definition -replace '\\\s*\r?\n', ' '
    $definition = $definition -replace '/\*.*?\*/', ' '
    $definition = $definition -replace '//.*', ' '
    $definition = $definition.Trim()
    $literalMatch = [regex]::Match($definition, '^\(\s*(0x[0-9A-Fa-f]+|[0-9]+)\s*(?:ULL|UL|LL|U|L)?\s*\)$')
    if (-not $literalMatch.Success) {
        $literalMatch = [regex]::Match($definition, '^(0x[0-9A-Fa-f]+|[0-9]+)\s*(?:ULL|UL|LL|U|L)?$')
    }
    if ($literalMatch.Success) {
        $literal = $literalMatch.Groups[1].Value
        if ($literal -match '^0x') {
            return [uint32] ([Convert]::ToUInt64($literal.Substring(2), 16))
        }
        return [uint32] ([Convert]::ToUInt64($literal, 10))
    }

    $expression = $definition
    while ($expression -match '\bRA8P1_[A-Za-z0-9_]+\b') {
        $identifier = $Matches[0]
        $value = Get-IntegerMacro -Text $Text -Name $identifier -Stack ($Stack + $Name)
        $expression = [regex]::Replace($expression,
                                       "\b$([regex]::Escape($identifier))\b",
                                       [string]$value)
    }
    $expression = $expression -replace '(?i)(ULL|UL|LL|U|L)\b', ''
    $expression = $expression.Trim()
    if ($expression -notmatch '^[0-9\s()+*/%\-]+$') {
        throw "Unsupported numeric macro expression for ${Name}: $definition"
    }
    try {
        $result = [System.Data.DataTable]::new().Compute($expression, $null)
        return [uint32] ([int64] $result)
    }
    catch {
        throw "Could not evaluate numeric macro ${Name}`: $definition"
    }
}

function Initialize-Layout {
    $resourcePath = Join-Path $script:ProjectRoot 'shared\resource_layout.h'
    $streamPath = Join-Path $script:ProjectRoot 'shared\display_stream.h'
    $tilePath = Join-Path $script:ProjectRoot 'shared\display_tile.h'
    $ipcPath = Join-Path $script:ProjectRoot 'shared\ipc_mailbox.h'
    foreach ($path in @($resourcePath, $streamPath, $tilePath, $ipcPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Shared protocol header not found: $path"
        }
    }

    $resource = Get-Content -Raw -LiteralPath $resourcePath
    $stream = Get-Content -Raw -LiteralPath $streamPath
    $tile = Get-Content -Raw -LiteralPath $tilePath
    $ipc = Get-Content -Raw -LiteralPath $ipcPath

    $sharedBase = Get-IntegerMacro $resource 'RA8P1_SHARED_RAM_BASE'
    $displayOffset = Get-IntegerMacro $resource 'RA8P1_DISPLAY_STREAM_OFFSET'
    $displayBytes = Get-IntegerMacro $resource 'RA8P1_DISPLAY_STREAM_BYTES'
    $tileOffset = Get-IntegerMacro $resource 'RA8P1_DISPLAY_TILE_OFFSET'
    $tileSlotBytes = Get-IntegerMacro $resource 'RA8P1_DISPLAY_TILE_SLOT_BYTES'
    $tileSlotCount = Get-IntegerMacro $resource 'RA8P1_DISPLAY_TILE_SLOT_COUNT'
    $tileBytes = Get-IntegerMacro $resource 'RA8P1_DISPLAY_TILE_BYTES'
    $commandOffset = Get-IntegerMacro $resource 'RA8P1_IPC_COMMAND_OFFSET'
    $runtimeOffset = Get-IntegerMacro $resource 'RA8P1_IPC_RUNTIME_OFFSET'
    $controlBytes = Get-IntegerMacro $stream 'RA8P1_DISPLAY_STREAM_CONTROL_BYTES'
    $slotCount = Get-IntegerMacro $stream 'RA8P1_DISPLAY_STREAM_SLOT_COUNT'
    $slotBytes = Get-IntegerMacro $stream 'RA8P1_DISPLAY_STREAM_SLOT_BYTES'
    $streamMagic = Get-IntegerMacro $stream 'RA8P1_DISPLAY_STREAM_MAGIC'
    $streamVersion = Get-IntegerMacro $stream 'RA8P1_DISPLAY_STREAM_VERSION'
    $tileMagic = Get-IntegerMacro $tile 'RA8P1_DISPLAY_TILE_MAGIC'
    $tileVersion = Get-IntegerMacro $tile 'RA8P1_DISPLAY_TILE_VERSION'
    $tileWidth = Get-IntegerMacro $tile 'RA8P1_DISPLAY_TILE_WIDTH'
    $tileHeight = Get-IntegerMacro $tile 'RA8P1_DISPLAY_TILE_HEIGHT'
    $tileRowBytes = Get-IntegerMacro $tile 'RA8P1_DISPLAY_TILE_ROW_BYTES'
    $runtimeBytes = Get-IntegerMacro $ipc 'RA8P1_RUNTIME_STATUS_BYTES'
    $runtimeMetricsVersion = Get-IntegerMacro $ipc 'RA8P1_RUNTIME_METRICS_VERSION'

    if ($slotCount -ne 4) {
        throw "The sampler expects four display slots; header declares $slotCount."
    }
    if (($controlBytes + ($slotCount * $slotBytes)) -ne $displayBytes) {
        throw 'Display stream header sizes do not form a contiguous stream region.'
    }
    if (($tileSlotBytes -lt 16) -or ($tileSlotCount -lt $tileHeight) -or
        (($tileSlotCount -band ($tileSlotCount - 1)) -ne 0) -or
        (($tileOffset % 32) -ne 0) -or
        (($tileSlotBytes % 32) -ne 0) -or
        (($tileSlotBytes * $tileSlotCount) -ne $tileBytes) -or
        ($tileOffset -lt ($displayOffset + $displayBytes)) -or
        (($tileOffset + $tileBytes) -gt $commandOffset)) {
        throw 'Display tile slot region is misaligned, overlapping, or out of bounds.'
    }
    if (($tileRowBytes -ne $tileWidth) -or
        (($tileSlotBytes - 8) -lt (36 + $tileRowBytes))) {
        throw 'Display tile payload is not a complete compact frequency row.'
    }
    if (($runtimeBytes -lt 128) -or (($runtimeBytes % 4) -ne 0)) {
        throw 'Runtime status size must be at least 128 bytes and word aligned.'
    }
    $readStart = Add-Address $sharedBase $displayOffset
    $readEnd = Add-Address $sharedBase ($tileOffset + $tileBytes)
    $readBytes = [uint64] $readEnd - [uint64] $readStart
    if (($readBytes % 4) -ne 0) {
        throw 'The display/tile read range is not word aligned.'
    }

    $script:HeaderValues = [ordered]@{
        ResourceHeader = $resourcePath
        StreamHeader = $streamPath
        TileHeader = $tilePath
        StreamMagic = $streamMagic
        StreamVersion = $streamVersion
        TileMagic = $tileMagic
        TileVersion = $tileVersion
        RuntimeMetricsVersion = $runtimeMetricsVersion
    }
    $script:Layout = [ordered]@{
        SharedBase = $sharedBase
        DisplayControl = $readStart
        DisplayControlBytes = $controlBytes
        DisplayStreamBytes = $displayBytes
        SlotsBase = Add-Address $readStart $controlBytes
        SlotCount = $slotCount
        SlotBytes = $slotBytes
        TileBase = Add-Address $sharedBase $tileOffset
        TileBytes = $tileBytes
        TileSlotCount = $tileSlotCount
        TileSlotBytes = $tileSlotBytes
        DisplayReadStart = $readStart
        DisplayReadBytes = [uint32] $readBytes
        DisplayReadWords = [int] ($readBytes / 4)
        Runtime = Add-Address $sharedBase $runtimeOffset
        RuntimeBytes = $runtimeBytes
        RuntimeWords = [int] ($runtimeBytes / 4)
        FrameBytes = 504
        TilePayloadBytes = [uint32] ($tileSlotBytes - 8)
        TileWidth = $tileWidth
        TileHeight = $tileHeight
        TileRowBytes = $tileRowBytes
    }
}

function Read-HostConfig {
    $path = Join-Path $HOME '.codex\ra8p1.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return @{}
    }
    try {
        $object = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
        $config = @{}
        foreach ($property in $object.PSObject.Properties) {
            $config[$property.Name] = [string] $property.Value
        }
        return $config
    }
    catch {
        throw "Could not read host configuration $path`: $($_.Exception.Message)"
    }
}

function Resolve-Probe {
    param([string] $Requested, [hashtable] $Config)
    $candidate = if ($Requested) {
        $Requested
    }
    elseif ($env:RA8P1_PROBE_SERIAL) {
        $env:RA8P1_PROBE_SERIAL
    }
    else {
        [string] $Config['ProbeSerial']
    }
    if (-not $candidate) {
        throw 'J-Link serial is unknown. Pass -ProbeSerial or set RA8P1_PROBE_SERIAL.'
    }
    $candidate = $candidate.Trim()
    if ($candidate -notmatch '^[0-9]{6,20}$') {
        throw 'J-Link serial must contain 6 to 20 decimal digits.'
    }
    return $candidate.TrimStart('0')
}

function Resolve-JLink {
    param([string] $Requested, [hashtable] $Config)
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($Requested) { [void] $candidates.Add($Requested) }
    if ($env:RA8P1_JLINK_ROOT) {
        [void] $candidates.Add((Join-Path $env:RA8P1_JLINK_ROOT 'JLink.exe'))
    }
    if ($Config['JLinkRoot']) {
        [void] $candidates.Add((Join-Path $Config['JLinkRoot'] 'JLink.exe'))
    }
    $command = Get-Command JLink.exe -ErrorAction SilentlyContinue
    if ($command) { [void] $candidates.Add($command.Source) }
    $candidates.Add((Join-Path $HOME 'SEGGER\JLink_V958\JLink.exe'))
    $candidates.Add('C:\Program Files\SEGGER\JLink\JLink.exe')

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'JLink.exe was not found. Pass -JLinkExe or set RA8P1_JLINK_ROOT.'
}

function Get-PreferredElf {
    param([string] $Requested, [string] $Core)
    if ($Requested) { return $Requested }
    $project = if ($Core -eq 'CPU0') {
        $script:ProjectLayout.Cpu0Directory
    }
    else {
        $script:ProjectLayout.Cpu1Directory
    }
    $debug = Join-Path $project 'Debug'
    if (-not (Test-Path -LiteralPath $debug -PathType Container)) { return $null }
    $files = Get-ChildItem -LiteralPath $debug -Filter '*.elf' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notmatch '\.before-' } |
        Sort-Object LastWriteTime -Descending
    if ($files) { return $files[0].FullName }
    return $null
}

function Get-ElfRecord {
    param([string] $Path, [string] $Core)
    if (-not $Path) {
        return [ordered]@{ Core = $Core; Path = $null; Exists = $false; Sha256 = $null }
    }
    $full = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        return [ordered]@{ Core = $Core; Path = $full; Exists = $false; Sha256 = $null }
    }
    $hash = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToUpperInvariant()
    return [ordered]@{ Core = $Core; Path = $full; Exists = $true; Sha256 = $hash }
}

function ConvertTo-Int32Value {
    param([uint32] $Value)
    if ([uint64]$Value -ge [uint64]2147483648) {
        return [int64] $Value - [int64]4294967296
    }
    return [int64] $Value
}

function Get-Delta32 {
    param([uint32] $NewValue, [uint32] $OldValue)
    $new64 = [uint64] $NewValue
    $old64 = [uint64] $OldValue
    if ($new64 -ge $old64) { return $new64 - $old64 }
    return [uint64]4294967296 + $new64 - $old64
}

function Test-Newer32 {
    param([uint32] $Candidate, [uint32] $Current)
    $delta = Get-Delta32 $Candidate $Current
    return ($delta -ne 0) -and ($delta -lt [uint64]2147483648)
}

function Get-Word {
    param([hashtable] $Map, [uint32] $Address)
    $key = '{0:X8}' -f $Address
    if (-not $Map.ContainsKey($key)) {
        throw "Memory word $(Format-Address $Address) was not present in the J-Link output."
    }
    return [uint32] $Map[$key]
}

function Get-PackedVersion {
    param([uint32] $Word)
    return [uint32] ($Word -band 0xFFFF)
}

function Get-PackedSize {
    param([uint32] $Word)
    return [uint32] (($Word -shr 16) -band 0xFFFF)
}

function Get-Mem32Records {
    param([Parameter(Mandatory)] [string] $Text)
    $records = New-Object System.Collections.Generic.List[object]
    foreach ($line in ($Text -split "`r?`n")) {
        $match = [regex]::Match($line, '^\s*(?:J-Link>\s*)?([0-9A-Fa-f]{8})\s*=\s*(.+)$')
        if (-not $match.Success) { continue }
        $start = [uint32] ([Convert]::ToUInt64($match.Groups[1].Value, 16))
        $wordMatches = [regex]::Matches($match.Groups[2].Value, '(?<![0-9A-Fa-f])([0-9A-Fa-f]{8})(?![0-9A-Fa-f])')
        $index = 0
        foreach ($wordMatch in $wordMatches) {
            $address = [uint32] ([uint64] $start + [uint64] (4 * $index))
            $value = [uint32] ([Convert]::ToUInt64($wordMatch.Groups[1].Value, 16))
            [void] $records.Add([pscustomobject]@{ Address = $address; Value = $value })
            $index++
        }
    }
    return $records
}

function Group-Mem32Occurrences {
    param([Parameter(Mandatory)] $Records)
    $groups = @{}
    foreach ($record in $Records) {
        $key = '{0:X8}' -f ([uint32] $record.Address)
        if (-not $groups.ContainsKey($key)) {
            $groups[$key] = New-Object System.Collections.Generic.List[uint32]
        }
        [void] $groups[$key].Add([uint32] $record.Value)
    }
    return $groups
}

function Select-Mem32Occurrence {
    param([hashtable] $Groups, [int] $Occurrence)
    $map = @{}
    foreach ($key in $Groups.Keys) {
        $values = $Groups[$key]
        if ($values.Count -gt $Occurrence) {
            $map[$key] = [uint32] $values[$Occurrence]
        }
    }
    return $map
}

function Get-DisplaySnapshot {
    param([Parameter(Mandatory)] [hashtable] $Map)

    $layout = $script:Layout
    $control = $layout.DisplayControl
    $controlMagic = Get-Word $Map (Add-Address $control 0)
    $controlPacked = Get-Word $Map (Add-Address $control 4)
    $session = Get-Word $Map (Add-Address $control 8)
    $controlVersion = Get-PackedVersion $controlPacked
    $controlSize = Get-PackedSize $controlPacked
    $controlValid = ($controlMagic -eq $script:HeaderValues.StreamMagic) -and
                    ($controlVersion -eq $script:HeaderValues.StreamVersion) -and
                    ($controlSize -eq $layout.DisplayControlBytes) -and
                    ($session -ne 0)

    $slots = New-Object System.Collections.Generic.List[object]
    for ($index = 0; $index -lt $layout.SlotCount; $index++) {
        $base = Add-Address $layout.SlotsBase ($index * $layout.SlotBytes)
        $begin = Get-Word $Map (Add-Address $base 0)
        $end = Get-Word $Map (Add-Address $base ($layout.SlotBytes - 4))
        $framePacked = Get-Word $Map (Add-Address $base 8)
        $frameMagic = Get-Word $Map (Add-Address $base 4)
        $frameSession = Get-Word $Map (Add-Address $base 12)
        $frameSequence = Get-Word $Map (Add-Address $base 16)
        $frameVersion = Get-PackedVersion $framePacked
        $frameSize = Get-PackedSize $framePacked
        $seqlockValid = ($begin -ne 0) -and (($begin -band 1) -eq 0) -and ($begin -eq $end)
        $payloadValid = ($frameMagic -eq $script:HeaderValues.StreamMagic) -and
                        ($frameVersion -eq $script:HeaderValues.StreamVersion) -and
                        ($frameSize -eq $layout.FrameBytes) -and
                        ($frameSequence -eq $begin) -and
                        $controlValid -and ($frameSession -eq $session)
        $candidate = [ordered]@{
            Index = $index
            Address = (Format-Address $base)
            Begin = $begin
            End = $end
            SeqlockValid = $seqlockValid
            Magic = ('0x{0:X8}' -f $frameMagic)
            Version = $frameVersion
            Size = $frameSize
            SessionId = $frameSession
            Sequence = $frameSequence
            Valid = $seqlockValid -and $payloadValid
        }
        if ($seqlockValid) {
            $candidate['WindowSequence'] = Get-Word $Map (Add-Address $base 312)
            $candidate['WindowSampleCount'] = Get-Word $Map (Add-Address $base 324)
            $candidate['StftFrameCount'] = Get-Word $Map (Add-Address $base 328)
            $candidate['StftCycles'] = Get-Word $Map (Add-Address $base 332)
            $candidate['NpuCycles'] = Get-Word $Map (Add-Address $base 336)
            $candidate['EndToEndCycles'] = Get-Word $Map (Add-Address $base 340)
            $candidate['NpuInferenceCount'] = Get-Word $Map (Add-Address $base 344)
            $candidate['NpuClass'] = Get-Word $Map (Add-Address $base 348)
            $candidate['NpuScoreQ15'] = ConvertTo-Int32Value (Get-Word $Map (Add-Address $base 352))
            $candidate['QueueDepth'] = Get-Word $Map (Add-Address $base 356)
            $candidate['IngressDrops'] = Get-Word $Map (Add-Address $base 360)
            $candidate['NpuReady'] = Get-Word $Map (Add-Address $base 364)
            $candidate['Flags'] = Get-Word $Map (Add-Address $base 32)
            $candidate['PublishTick'] = Get-Word $Map (Add-Address $base 308)
            $candidate['SampleRateHz'] = Get-Word $Map (Add-Address $base 20)
            $candidate['SourceSampleRateHz'] = Get-Word $Map (Add-Address $base 384)
            $candidate['ValidBits'] = Get-Word $Map (Add-Address $base 388)
            $candidate['TimingFlags'] = Get-Word $Map (Add-Address $base 488)
            $candidate['StftMs'] = [math]::Round(([double] $candidate['StftCycles'] / 1000000.0), 6)
            $candidate['NpuMs'] = [math]::Round(([double] $candidate['NpuCycles'] / 1000000.0), 6)
            $candidate['EndToEndMs'] = [math]::Round(([double] $candidate['EndToEndCycles'] / 1000000.0), 6)
        }
        [void] $slots.Add([pscustomobject] $candidate)
    }

    $validSlots = @($slots | Where-Object { $_.Valid })
    $latest = $null
    foreach ($slot in $validSlots) {
        if (($null -eq $latest) -or (Test-Newer32 $slot.Sequence $latest.Sequence)) {
            $latest = $slot
        }
    }

    $runtime = $layout.Runtime
    $runtimeBegin = Get-Word $Map (Add-Address $runtime 0)
    $runtimeHeartbeat = Get-Word $Map (Add-Address $runtime 4)
    $runtimeStage = Get-Word $Map (Add-Address $runtime 8)
    $runtimeLines = Get-Word $Map (Add-Address $runtime 12)
    $runtimeError = ConvertTo-Int32Value (Get-Word $Map (Add-Address $runtime 16))
    $runtimeEnd = Get-Word $Map (Add-Address $runtime 20)
    $runtimeUnderflows = Get-Word $Map (Add-Address $runtime 24)
    $runtimeRunning = Get-Word $Map (Add-Address $runtime 28)
    $runtimeMetricsVersion = Get-Word $Map (Add-Address $runtime 32)
    $runtimeLvglTick = Get-Word $Map (Add-Address $runtime 36)
    $runtimePresentedFrames = Get-Word $Map (Add-Address $runtime 40)
    $runtimePresentedFps = Get-Word $Map (Add-Address $runtime 44)
    $runtimeUnderflowRate = Get-Word $Map (Add-Address $runtime 48)
    $runtimeWindowRate = Get-Word $Map (Add-Address $runtime 52)
    $runtimeInferenceRate = Get-Word $Map (Add-Address $runtime 56)
    $runtimeTileRate = Get-Word $Map (Add-Address $runtime 60)
    $runtimeContentFrames = Get-Word $Map (Add-Address $runtime 64)
    $runtimeContentFps = Get-Word $Map (Add-Address $runtime 68)
    $runtimeWaterfallColumns = Get-Word $Map (Add-Address $runtime 72)
    $runtimeWaterfallTiles = Get-Word $Map (Add-Address $runtime 76)
    $runtimeWaterfallDrops = Get-Word $Map (Add-Address $runtime 80)
    $runtimeIpcFrames = Get-Word $Map (Add-Address $runtime 84)
    $runtimeIpcTiles = Get-Word $Map (Add-Address $runtime 88)
    $runtimeIpcTilesMissed = Get-Word $Map (Add-Address $runtime 92)
    $runtimeLastSession = Get-Word $Map (Add-Address $runtime 96)
    $runtimeLastFrameSequence = Get-Word $Map (Add-Address $runtime 100)
    $runtimeLastTileSequence = Get-Word $Map (Add-Address $runtime 104)
    $runtimeLastCommandSequence = Get-Word $Map (Add-Address $runtime 108)
    $runtimeLastCommandStatus = Get-Word $Map (Add-Address $runtime 112)
    $runtimeLastCommandReason = Get-Word $Map (Add-Address $runtime 116)
    $runtimeLastAppliedSession = Get-Word $Map (Add-Address $runtime 120)
    $runtimeFlags = Get-Word $Map (Add-Address $runtime 124)
    $runtimeValid = ($runtimeBegin -ne 0) -and (($runtimeBegin -band 1) -eq 0) -and
                    ($runtimeBegin -eq $runtimeEnd)

    $tileCandidates = New-Object System.Collections.Generic.List[object]
    for ($index = 0; $index -lt $layout.TileSlotCount; $index++) {
        $tile = Add-Address $layout.TileBase ($index * $layout.TileSlotBytes)
        $tileBegin = Get-Word $Map (Add-Address $tile 0)
        $tileEnd = Get-Word $Map (Add-Address $tile ($layout.TileSlotBytes - 4))
        $tileMagic = Get-Word $Map (Add-Address $tile 4)
        $tilePacked = Get-Word $Map (Add-Address $tile 8)
        $tileSession = Get-Word $Map (Add-Address $tile 12)
        $tileSequence = Get-Word $Map (Add-Address $tile 16)
        $tileWindowSequence = Get-Word $Map (Add-Address $tile 20)
        $tileWidthHeight = Get-Word $Map (Add-Address $tile 24)
        $tileFlags = Get-Word $Map (Add-Address $tile 28)
        $tileMetadata = Get-Word $Map (Add-Address $tile 32)
        $tileNovelCountWord = Get-Word $Map (Add-Address $tile 36)
        $tileCenterIndex = $tileMetadata -band 0xFF
        $tileIndex = ($tileMetadata -shr 8) -band 0xFF
        $tileCount = ($tileMetadata -shr 16) -band 0xFF
        $tileNovelTimeStart = ($tileMetadata -shr 24) -band 0xFF
        $tileNovelTimeCount = $tileNovelCountWord -band 0xFF
        $tileSeqlockValid = ($tileBegin -ne 0) -and (($tileBegin -band 1) -eq 0) -and ($tileBegin -eq $tileEnd)
        $tileValid = $tileSeqlockValid -and
                     ($tileMagic -eq $script:HeaderValues.TileMagic) -and
                     ((Get-PackedVersion $tilePacked) -eq $script:HeaderValues.TileVersion) -and
                     ((Get-PackedSize $tilePacked) -eq $layout.TilePayloadBytes) -and
                     ($tileSequence -eq $tileBegin) -and
                     $controlValid -and ($tileSession -eq $session) -and
                     ($tileWidthHeight -eq (($layout.TileWidth -shl 16) -bor $layout.TileHeight)) -and
                     ($tileNovelCountWord -eq 1) -and
                     ($tileNovelTimeStart -lt $layout.TileHeight)
        [void] $tileCandidates.Add([pscustomobject]@{
            Index = $index
            Address = (Format-Address $tile)
            Begin = $tileBegin
            End = $tileEnd
            Sequence = $tileSequence
            WindowSequence = $tileWindowSequence
            SessionId = $tileSession
            Width = $layout.TileWidth
            Height = $layout.TileHeight
            WidthHeight = $tileWidthHeight
            Flags = $tileFlags
            CenterIndex = $tileCenterIndex
            TileIndex = $tileIndex
            TileCount = $tileCount
            NovelTimeStart = $tileNovelTimeStart
            NovelTimeCount = $tileNovelTimeCount
            RowBytes = $layout.TileRowBytes
            Valid = $tileValid
        })
    }
    $tile = $null
    foreach ($candidate in @($tileCandidates | Where-Object { $_.Valid })) {
        if (($null -eq $tile) -or (Test-Newer32 $candidate.Sequence $tile.Sequence)) {
            $tile = $candidate
        }
    }
    $tileValid = ($null -ne $tile)

    return [pscustomobject]@{
        ControlValid = $controlValid
        ControlMagic = ('0x{0:X8}' -f $controlMagic)
        ControlVersion = $controlVersion
        ControlSize = $controlSize
        SessionId = $session
        Slots = @($slots | ForEach-Object { $_ })
        ValidSlotCount = $validSlots.Count
        Latest = $latest
        Runtime = [pscustomobject]@{
            Valid = $runtimeValid
            Begin = $runtimeBegin
            End = $runtimeEnd
            Sequence = $runtimeBegin
            Heartbeat = $runtimeHeartbeat
            Stage = $runtimeStage
            LineEvents = $runtimeLines
            LastError = $runtimeError
            Underflows = $runtimeUnderflows
            Running = $runtimeRunning
            MetricsVersion = $runtimeMetricsVersion
            LvglTickMs = $runtimeLvglTick
            PresentedFrameCount = $runtimePresentedFrames
            PresentedFpsMillihz = $runtimePresentedFps
            PresentedFpsHz = [math]::Round(([double]$runtimePresentedFps / 1000.0), 3)
            UnderflowRateMillihz = $runtimeUnderflowRate
            UnderflowRateHz = [math]::Round(([double]$runtimeUnderflowRate / 1000.0), 3)
            WindowRateMillihz = $runtimeWindowRate
            WindowRateHz = [math]::Round(([double]$runtimeWindowRate / 1000.0), 3)
            InferenceRateMillihz = $runtimeInferenceRate
            InferenceRateHz = [math]::Round(([double]$runtimeInferenceRate / 1000.0), 3)
            TileRateMillihz = $runtimeTileRate
            TileRateHz = [math]::Round(([double]$runtimeTileRate / 1000.0), 3)
            ContentFrameCount = $runtimeContentFrames
            ContentFpsMillihz = $runtimeContentFps
            ContentFpsHz = [math]::Round(([double]$runtimeContentFps / 1000.0), 3)
            WaterfallColumnsGenerated = $runtimeWaterfallColumns
            WaterfallTilesConsumed = $runtimeWaterfallTiles
            WaterfallTilesDropped = $runtimeWaterfallDrops
            IpcFramesReceived = $runtimeIpcFrames
            IpcTilesReceived = $runtimeIpcTiles
            IpcTilesMissed = $runtimeIpcTilesMissed
            LastSessionId = $runtimeLastSession
            LastFrameSequence = $runtimeLastFrameSequence
            LastTileSequence = $runtimeLastTileSequence
            LastCommandSequence = $runtimeLastCommandSequence
            LastCommandStatus = $runtimeLastCommandStatus
            LastCommandReason = $runtimeLastCommandReason
            LastAppliedSessionId = $runtimeLastAppliedSession
            RuntimeFlags = $runtimeFlags
            HasSession = (($runtimeFlags -band (1 -shl 0)) -ne 0)
            HasIpcContent = (($runtimeFlags -band (1 -shl 1)) -ne 0)
            DisplayStopped = (($runtimeFlags -band (1 -shl 2)) -ne 0)
            ModelPlaceholder = (($runtimeFlags -band (1 -shl 3)) -ne 0)
            Cpu0Ready = (($runtimeFlags -band (1 -shl 4)) -ne 0)
            CommandPending = (($runtimeFlags -band (1 -shl 5)) -ne 0)
            CommandRetried = (($runtimeFlags -band (1 -shl 6)) -ne 0)
            LiveRetryScheduled = (($runtimeFlags -band (1 -shl 7)) -ne 0)
            CenterValidMask = (($runtimeFlags -shr 8) -band 0x0F)
        }
        Tile = [pscustomobject]@{
            Valid = $tileValid
            SeqlockValid = $tileValid
            Address = if ($tileValid) { $tile.Address } else { (Format-Address $layout.TileBase) }
            Begin = if ($tileValid) { $tile.Begin } else { 0 }
            End = if ($tileValid) { $tile.End } else { 0 }
            Sequence = if ($tileValid) { $tile.Sequence } else { 0 }
            WindowSequence = if ($tileValid) { $tile.WindowSequence } else { 0 }
            SessionId = if ($tileValid) { $tile.SessionId } else { 0 }
            Width = $layout.TileWidth
            Height = $layout.TileHeight
            WidthHeight = if ($tileValid) { $tile.WidthHeight } else { 0 }
            Flags = if ($tileValid) { $tile.Flags } else { 0 }
            CenterIndex = if ($tileValid) { $tile.CenterIndex } else { 0 }
            TileIndex = if ($tileValid) { $tile.TileIndex } else { 0 }
            TileCount = if ($tileValid) { $tile.TileCount } else { 0 }
            NovelTimeStart = if ($tileValid) { $tile.NovelTimeStart } else { 0 }
            NovelTimeCount = if ($tileValid) { $tile.NovelTimeCount } else { 0 }
            RowBytes = $layout.TileRowBytes
        }
    }
}

function Get-IntervalReport {
    param(
        [Parameter(Mandatory)] $First,
        [Parameter(Mandatory)] $Last,
        [double] $HostElapsedSeconds,
        [double] $MeasurementElapsedSeconds = 0
    )

    $sessionChanged = (-not $First.ControlValid) -or (-not $Last.ControlValid) -or
                      ($First.SessionId -ne $Last.SessionId)
    $result = [ordered]@{
        SessionChanged = $sessionChanged
        HostElapsedSeconds = [math]::Round($HostElapsedSeconds, 3)
        MeasurementElapsedSeconds = [math]::Round($MeasurementElapsedSeconds, 3)
        HeartbeatDelta = $null
        HeartbeatRateHz = $null
        RuntimeAdvanced = $false
        WindowDelta = $null
        DisplayFrameDelta = $null
        TileDelta = $null
        InferenceDelta = $null
        UnderflowDelta = $null
        WindowRateHz = $null
        StftFrameRateHz = $null
        DisplayFrameRateHz = $null
        TileRateHz = $null
        InferenceRateHz = $null
        UnderflowRateHz = $null
        CounterResetSuspected = $false
    }

    if ($First.Runtime.Valid -and $Last.Runtime.Valid) {
        $heartbeatDelta = Get-Delta32 $Last.Runtime.Heartbeat $First.Runtime.Heartbeat
        $lineDelta = Get-Delta32 $Last.Runtime.LineEvents $First.Runtime.LineEvents
        $runtimeSequenceDelta = Get-Delta32 $Last.Runtime.Sequence $First.Runtime.Sequence
        $underflowDelta = Get-Delta32 $Last.Runtime.Underflows $First.Runtime.Underflows
        $result.HeartbeatDelta = $heartbeatDelta
        $result.RuntimeAdvanced = ($heartbeatDelta -ne 0) -or
                                  ($lineDelta -ne 0) -or
                                  ($runtimeSequenceDelta -ne 0)
        if ($underflowDelta -gt [uint64]2147483648) {
            $result.CounterResetSuspected = $true
        }
        else {
            $result.UnderflowDelta = $underflowDelta
        }
    }

    if ((-not $sessionChanged) -and $First.Latest -and $Last.Latest) {
        $windowDelta = Get-Delta32 $Last.Latest.WindowSequence $First.Latest.WindowSequence
        $displayDeltaRaw = Get-Delta32 $Last.Latest.Sequence $First.Latest.Sequence
        $inferenceDelta = Get-Delta32 $Last.Latest.NpuInferenceCount $First.Latest.NpuInferenceCount
        $result.WindowDelta = $windowDelta
        $result.DisplayFrameDelta = [uint64] ($displayDeltaRaw / 2)
        $result.InferenceDelta = $inferenceDelta
        if ($First.Tile.Valid -and $Last.Tile.Valid -and
            ($First.Tile.SessionId -eq $Last.Tile.SessionId)) {
            $tileDeltaRaw = Get-Delta32 $Last.Tile.Sequence $First.Tile.Sequence
            $result.TileDelta = [uint64] ($tileDeltaRaw / 2)
        }
    }

    $seconds = if ($MeasurementElapsedSeconds -gt 0) {
        $MeasurementElapsedSeconds
    }
    else {
        $HostElapsedSeconds
    }
    if (($seconds -gt 0) -and (-not $result.CounterResetSuspected)) {
        if ($null -ne $result.HeartbeatDelta) {
            $result.HeartbeatRateHz = [math]::Round(([double] $result.HeartbeatDelta / $seconds), 3)
        }
        if ($null -ne $result.WindowDelta) {
            $result.WindowRateHz = [math]::Round(([double] $result.WindowDelta / $seconds), 3)
            if ($Last.Latest) {
                $result.StftFrameRateHz = [math]::Round((([double] $result.WindowDelta *
                    [double] $Last.Latest.StftFrameCount) / $seconds), 3)
            }
            $result.DisplayFrameRateHz = [math]::Round(([double] $result.DisplayFrameDelta / $seconds), 3)
            if ($null -ne $result.TileDelta) {
                $result.TileRateHz = [math]::Round(([double] $result.TileDelta / $seconds), 3)
            }
            $result.InferenceRateHz = [math]::Round(([double] $result.InferenceDelta / $seconds), 3)
        }
        if ($null -ne $result.UnderflowDelta -and $result.RuntimeAdvanced) {
            $result.UnderflowRateHz = [math]::Round(([double] $result.UnderflowDelta / $seconds), 3)
        }
    }
    return [pscustomobject] $result
}

function New-JLinkCommands {
    param(
        [Parameter(Mandatory)] [string] $Serial,
        [Parameter(Mandatory)] [int] $Seconds
    )
    $displayAddress = Format-Address $script:Layout.DisplayReadStart
    $runtimeAddress = Format-Address $script:Layout.Runtime
    $readDisplay = "mem32 $displayAddress $($script:Layout.DisplayReadWords)"
    $readRuntime = "mem32 $runtimeAddress $($script:Layout.RuntimeWords)"
    $commands = New-Object System.Collections.Generic.List[string]
    foreach ($command in @(
        "SelectEmuBySN $Serial",
        'device R7KA8P1KF_CPU0',
        'si SWD',
        'speed 4000',
        'connect',
        'halt',
        $readDisplay,
        $readRuntime
    )) { [void] $commands.Add($command) }
    if ($Seconds -gt 0) {
        [void] $commands.Add('go')
        [void] $commands.Add("sleep $($Seconds * 1000)")
        [void] $commands.Add('halt')
        [void] $commands.Add($readDisplay)
        [void] $commands.Add($readRuntime)
    }
    [void] $commands.Add('go')
    [void] $commands.Add('exit')
    return $commands
}

function Invoke-JLinkRead {
    param(
        [Parameter(Mandatory)] [string] $Executable,
        [Parameter(Mandatory)] [string[]] $Commands
    )
    $inputText = ($Commands -join "`r`n") + "`r`n"
    $lines = New-Object System.Collections.Generic.List[string]
    $saved = $ErrorActionPreference
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        $ErrorActionPreference = 'Continue'
        (($inputText | & $Executable -NoGui 1 2>&1) | ForEach-Object {
            [void] $lines.Add([string] $_)
        })
        $exitCode = $LASTEXITCODE
    }
    catch {
        [void] $lines.Add($_.Exception.Message)
        $exitCode = -1
    }
    finally {
        $watch.Stop()
        $ErrorActionPreference = $saved
    }
    return [pscustomobject]@{
        Text = ($lines -join "`n")
        ExitCode = $exitCode
        HostElapsedSeconds = $watch.Elapsed.TotalSeconds
    }
}

function Assert-JLinkEvidence {
    param(
        [Parameter(Mandatory)] [string] $Text,
        [Parameter(Mandatory)] [int] $ExitCode,
        [Parameter(Mandatory)] [string] $Serial
    )
    if ($ExitCode -ne 0) { throw "J-Link Commander failed with exit code $ExitCode.`n$Text" }
    if ($Text -match 'Cannot connect|Failed to connect|Could not find emulator|No emulator connected|Unknown command') {
        throw "J-Link connection failed.`n$Text"
    }
    $serialPattern = 'S/N:\s*0*' + [regex]::Escape($Serial) + '\s*$'
    if ($Text -notmatch "(?m)^\s*(?:J-Link>\s*)?$serialPattern") {
        throw "J-Link output did not prove probe serial $Serial.`n$Text"
    }
    if ($Text -notmatch '(?m)^\s*(?:J-Link>\s*)?Device\s+"R7KA8P1KF_CPU0"\s+selected\.') {
        throw 'J-Link output did not prove R7KA8P1KF_CPU0 selection.'
    }
    if ($Text -notmatch 'VTref=[0-9.]+V' -or $Text -match 'VTref=0\.000V') {
        throw 'J-Link output did not prove a nonzero target voltage.'
    }
    if ($Text -notmatch 'Cortex-M85 identified') {
        throw 'J-Link output did not prove the CPU0 Cortex-M85 target.'
    }
}

function New-FakeMemory {
    return @{}
}

function Set-FakeWord {
    param([hashtable] $Memory, [uint32] $Address, [uint32] $Value)
    $Memory[('{0:X8}' -f $Address)] = $Value
}

function Get-FakeWord {
    param([hashtable] $Memory, [uint32] $Address)
    $key = '{0:X8}' -f $Address
    if ($Memory.ContainsKey($key)) { return [uint32] $Memory[$key] }
    return [uint32]0
}

function New-FakeMem32Text {
    param(
        [hashtable] $Memory,
        [uint32] $Start,
        [int] $Count
    )
    $lines = New-Object System.Collections.Generic.List[string]
    for ($offset = 0; $offset -lt $Count; $offset += 8) {
        $lineCount = [math]::Min(8, $Count - $offset)
        $words = New-Object System.Collections.Generic.List[string]
        for ($index = 0; $index -lt $lineCount; $index++) {
            $address = [uint32] ([uint64] $Start + [uint64] (4 * ($offset + $index)))
            [void] $words.Add(('{0:X8}' -f (Get-FakeWord $Memory $address)))
        }
        $lineAddress = [uint32] ([uint64] $Start + [uint64] (4 * $offset))
        $lineText = '{0:X8} = {1}' -f $lineAddress, ($words -join ' ')
        [void] $lines.Add($lineText)
    }
    return ($lines -join "`n")
}

function Set-FakeFrame {
    param(
        [hashtable] $Memory,
        [uint32] $Base,
        [uint32] $Session,
        [uint32] $Sequence,
        [uint32] $WindowSequence,
        [uint32] $WindowSamples,
        [uint32] $StftFrames,
        [uint32] $StftCycles,
        [uint32] $NpuCycles,
        [uint32] $E2eCycles,
        [uint32] $InferenceCount,
        [uint32] $BeginOverride = 0,
        [uint32] $EndOverride = 0
    )
    $begin = if ($BeginOverride -ne 0) { $BeginOverride } else { $Sequence }
    $end = if ($EndOverride -ne 0) { $EndOverride } else { $Sequence }
    Set-FakeWord $Memory (Add-Address $Base 0) $begin
    Set-FakeWord $Memory (Add-Address $Base 4) $script:HeaderValues.StreamMagic
    Set-FakeWord $Memory (Add-Address $Base 8) (($script:Layout.FrameBytes -shl 16) -bor $script:HeaderValues.StreamVersion)
    Set-FakeWord $Memory (Add-Address $Base 12) $Session
    Set-FakeWord $Memory (Add-Address $Base 16) $Sequence
    Set-FakeWord $Memory (Add-Address $Base 20) 60003333
    Set-FakeWord $Memory (Add-Address $Base 32) 2
    Set-FakeWord $Memory (Add-Address $Base 308) 1234
    Set-FakeWord $Memory (Add-Address $Base 312) $WindowSequence
    Set-FakeWord $Memory (Add-Address $Base 324) $WindowSamples
    Set-FakeWord $Memory (Add-Address $Base 328) $StftFrames
    Set-FakeWord $Memory (Add-Address $Base 332) $StftCycles
    Set-FakeWord $Memory (Add-Address $Base 336) $NpuCycles
    Set-FakeWord $Memory (Add-Address $Base 340) $E2eCycles
    Set-FakeWord $Memory (Add-Address $Base 344) $InferenceCount
    Set-FakeWord $Memory (Add-Address $Base 488) 7
    Set-FakeWord $Memory (Add-Address $Base ($script:Layout.SlotBytes - 4)) $end
}

function Set-FakeTile {
    param([hashtable] $Memory, [uint32] $Session, [uint32] $Sequence, [uint32] $WindowSequence)
    $slotIndex = ((($Sequence -shr 1) - 1) -band ($script:Layout.TileSlotCount - 1))
    $base = Add-Address $script:Layout.TileBase ($slotIndex * $script:Layout.TileSlotBytes)
    Set-FakeWord $Memory (Add-Address $base 0) $Sequence
    Set-FakeWord $Memory (Add-Address $base 4) $script:HeaderValues.TileMagic
    Set-FakeWord $Memory (Add-Address $base 8) (($script:Layout.TilePayloadBytes -shl 16) -bor $script:HeaderValues.TileVersion)
    Set-FakeWord $Memory (Add-Address $base 12) $Session
    Set-FakeWord $Memory (Add-Address $base 16) $Sequence
    Set-FakeWord $Memory (Add-Address $base 20) $WindowSequence
    Set-FakeWord $Memory (Add-Address $base 24) (($script:Layout.TileWidth -shl 16) -bor $script:Layout.TileHeight)
    Set-FakeWord $Memory (Add-Address $base 28) 2
    Set-FakeWord $Memory (Add-Address $base 32) 0x07130302
    Set-FakeWord $Memory (Add-Address $base 36) 1
    Set-FakeWord $Memory (Add-Address $base ($script:Layout.TileSlotBytes - 4)) $Sequence
}

function Set-FakeRuntime {
    param([hashtable] $Memory, [uint32] $Sequence, [uint32] $Heartbeat, [uint32] $Underflows)
    $base = $script:Layout.Runtime
    Set-FakeWord $Memory (Add-Address $base 0) $Sequence
    Set-FakeWord $Memory (Add-Address $base 4) $Heartbeat
    Set-FakeWord $Memory (Add-Address $base 8) 6
    Set-FakeWord $Memory (Add-Address $base 12) 100
    Set-FakeWord $Memory (Add-Address $base 16) 0
    Set-FakeWord $Memory (Add-Address $base 20) $Sequence
    Set-FakeWord $Memory (Add-Address $base 24) $Underflows
    Set-FakeWord $Memory (Add-Address $base 28) 1
    Set-FakeWord $Memory (Add-Address $base 32) $script:HeaderValues.RuntimeMetricsVersion
    Set-FakeWord $Memory (Add-Address $base 36) ($Heartbeat * 10)
    Set-FakeWord $Memory (Add-Address $base 40) ($Heartbeat * 2)
    Set-FakeWord $Memory (Add-Address $base 44) 37894
    Set-FakeWord $Memory (Add-Address $base 48) 0
    Set-FakeWord $Memory (Add-Address $base 52) 1700
    Set-FakeWord $Memory (Add-Address $base 56) 1700
    Set-FakeWord $Memory (Add-Address $base 60) 1700
    Set-FakeWord $Memory (Add-Address $base 64) 77
    Set-FakeWord $Memory (Add-Address $base 68) 46500
    Set-FakeWord $Memory (Add-Address $base 72) 6400
    Set-FakeWord $Memory (Add-Address $base 76) 400
    Set-FakeWord $Memory (Add-Address $base 80) 2
    Set-FakeWord $Memory (Add-Address $base 84) 11
    Set-FakeWord $Memory (Add-Address $base 88) 22
    Set-FakeWord $Memory (Add-Address $base 92) 3
    Set-FakeWord $Memory (Add-Address $base 96) 0x12345678
    Set-FakeWord $Memory (Add-Address $base 100) 10
    Set-FakeWord $Memory (Add-Address $base 104) 14
    Set-FakeWord $Memory (Add-Address $base 108) 5
    Set-FakeWord $Memory (Add-Address $base 112) 2
    Set-FakeWord $Memory (Add-Address $base 116) 12
    Set-FakeWord $Memory (Add-Address $base 120) 0
    Set-FakeWord $Memory (Add-Address $base 124) 0x00000FDB
}

function Assert-SelfTest {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw "Self-test failed: $Message" }
}

function Invoke-SelfTest {
    $session = [uint32]7
    $memory = New-FakeMemory
    Set-FakeWord $memory (Add-Address $script:Layout.DisplayControl 0) $script:HeaderValues.StreamMagic
    Set-FakeWord $memory (Add-Address $script:Layout.DisplayControl 4) (($script:Layout.DisplayControlBytes -shl 16) -bor $script:HeaderValues.StreamVersion)
    Set-FakeWord $memory (Add-Address $script:Layout.DisplayControl 8) $session
    for ($index = 0; $index -lt $script:Layout.SlotCount; $index++) {
        $base = Add-Address $script:Layout.SlotsBase ($index * $script:Layout.SlotBytes)
        Set-FakeFrame $memory $base $session 0 0 0 0 0 0 0 0
    }
    Set-FakeFrame $memory (Add-Address $script:Layout.SlotsBase 0) $session 2 10 600033 1171 1000000 200000 1500000 1
    Set-FakeFrame -Memory $memory `
        -Base (Add-Address $script:Layout.SlotsBase $script:Layout.SlotBytes) `
        -Session $session -Sequence 4 -WindowSequence 0 -WindowSamples 0 `
        -StftFrames 0 -StftCycles 0 -NpuCycles 0 -E2eCycles 0 `
        -InferenceCount 0 -BeginOverride 5 -EndOverride 4
    Set-FakeFrame $memory (Add-Address $script:Layout.SlotsBase (2 * $script:Layout.SlotBytes)) $session 6 12 600033 1171 1100000 220000 1600000 2
    Set-FakeFrame $memory (Add-Address $script:Layout.SlotsBase (3 * $script:Layout.SlotBytes)) 8 100 99 1 1 1 1 1
    Set-FakeTile $memory $session 6 12
    Set-FakeRuntime $memory 2 1000 3
    $text1 = (New-FakeMem32Text $memory $script:Layout.DisplayReadStart $script:Layout.DisplayReadWords) + "`n" +
             (New-FakeMem32Text $memory $script:Layout.Runtime $script:Layout.RuntimeWords)

    Set-FakeFrame $memory (Add-Address $script:Layout.SlotsBase 0) $session 10 16 600033 1171 1200000 240000 1700000 5
    Set-FakeFrame $memory (Add-Address $script:Layout.SlotsBase (2 * $script:Layout.SlotBytes)) $session 12 18 600033 1171 1300000 260000 1800000 6
    Set-FakeTile $memory $session 12 18
    Set-FakeRuntime $memory 4 2000 7
    $text2 = (New-FakeMem32Text $memory $script:Layout.DisplayReadStart $script:Layout.DisplayReadWords) + "`n" +
             (New-FakeMem32Text $memory $script:Layout.Runtime $script:Layout.RuntimeWords)

    $records = Get-Mem32Records ($text1 + "`n" + $text2)
    $groups = Group-Mem32Occurrences $records
    $first = Get-DisplaySnapshot (Select-Mem32Occurrence $groups 0)
    $last = Get-DisplaySnapshot (Select-Mem32Occurrence $groups 1)
    $interval = Get-IntervalReport $first $last 1.0 1.0

    Assert-SelfTest ($first.ControlValid) 'control validation'
    Assert-SelfTest ($first.ValidSlotCount -eq 2) 'session filtering and seqlock filtering'
    Assert-SelfTest ($first.Latest.Sequence -eq 6) 'latest display slot selection'
    Assert-SelfTest ($first.Latest.WindowSequence -eq 12) 'window sequence parsing'
    Assert-SelfTest ($first.Latest.StftCycles -eq 1100000) 'STFT cycle offset'
    Assert-SelfTest ($first.Latest.NpuCycles -eq 220000) 'NPU cycle offset'
    Assert-SelfTest ($first.Latest.EndToEndCycles -eq 1600000) 'E2E cycle offset'
    Assert-SelfTest ($first.Latest.TimingFlags -eq 7) 'timing-valid flags'
    Assert-SelfTest ($first.Tile.Valid -and ($first.Tile.Sequence -eq 6)) 'tile seqlock and sequence'
    Assert-SelfTest (($first.Tile.CenterIndex -eq 2) -and
                     ($first.Tile.TileIndex -eq 3) -and
                     ($first.Tile.TileCount -eq 19) -and
                     ($first.Tile.NovelTimeStart -eq 7) -and
                     ($first.Tile.NovelTimeCount -eq 1) -and
                     ($first.Tile.RowBytes -eq $script:Layout.TileRowBytes)) 'compact tile-row metadata parsing'
    Assert-SelfTest ($first.Runtime.Valid -and ($first.Runtime.Stage -eq 6)) 'runtime status parsing'
    Assert-SelfTest ($first.Runtime.PresentedFpsMillihz -eq 37894) 'self-timed FPS parsing'
    Assert-SelfTest ($first.Runtime.ContentFpsMillihz -eq 46500) 'real-content FPS parsing'
    Assert-SelfTest ($first.Runtime.WaterfallColumnsGenerated -eq 6400) 'waterfall column parsing'
    Assert-SelfTest ($first.Runtime.WaterfallTilesConsumed -eq 400) 'waterfall tile parsing'
    Assert-SelfTest ($first.Runtime.WaterfallTilesDropped -eq 2) 'waterfall drop parsing'
    Assert-SelfTest (($first.Runtime.IpcFramesReceived -eq 11) -and
                     ($first.Runtime.IpcTilesReceived -eq 22) -and
                     ($first.Runtime.IpcTilesMissed -eq 3)) 'runtime IPC counter parsing'
    Assert-SelfTest (($first.Runtime.LastSessionId -eq 0x12345678) -and
                     ($first.Runtime.LastFrameSequence -eq 10) -and
                     ($first.Runtime.LastTileSequence -eq 14)) 'runtime identity parsing'
    Assert-SelfTest (($first.Runtime.LastCommandSequence -eq 5) -and
                     ($first.Runtime.LastCommandStatus -eq 2) -and
                     ($first.Runtime.LastCommandReason -eq 12) -and
                     ($first.Runtime.LastAppliedSessionId -eq 0)) 'runtime command parsing'
    Assert-SelfTest (($first.Runtime.RuntimeFlags -eq 0x00000FDB) -and
                     $first.Runtime.Cpu0Ready -and
                     $first.Runtime.LiveRetryScheduled -and
                     ($first.Runtime.CenterValidMask -eq 15)) 'runtime flag parsing'
    Assert-SelfTest ($interval.WindowDelta -eq 6) 'window delta'
    Assert-SelfTest ($interval.InferenceDelta -eq 4) 'inference delta'
    Assert-SelfTest ($interval.UnderflowDelta -eq 4) 'underflow delta'
    Assert-SelfTest ($interval.WindowRateHz -eq 6) 'window rate'
    Assert-SelfTest ($interval.InferenceRateHz -eq 4) 'inference rate'
    Assert-SelfTest ($interval.UnderflowRateHz -eq 4) 'underflow rate'
    Write-Output 'Self-test passed: mem32 parser, seqlock, session selection, tile, counters and rates.'
}

function Write-HumanReport {
    param([Parameter(Mandatory)] $Report)
    Write-Output 'RA8P1 runtime sampler (read-only; J-Link CPU0)'
    Write-Output ("Probe={0}  Target={1}  Snapshots={2}" -f $Report.ProbeSerial, $Report.Target, $Report.SnapshotCount)
    Write-Output ("CPU0 ELF: {0}  SHA256={1}" -f $Report.Elf.Cpu0.Path, $Report.Elf.Cpu0.Sha256)
    Write-Output ("CPU1 ELF: {0}  SHA256={1}" -f $Report.Elf.Cpu1.Path, $Report.Elf.Cpu1.Sha256)
    Write-Output ("Layout: display={0} words={1}, runtime={2} words={3}, tile={4}" -f
        (Format-Address $script:Layout.DisplayReadStart), $script:Layout.DisplayReadWords,
        (Format-Address $script:Layout.Runtime), $script:Layout.RuntimeWords,
        (Format-Address $script:Layout.TileBase))
    $number = 1
    foreach ($snapshot in $Report.Snapshots) {
        Write-Output ("Snapshot {0}: control_valid={1} session=0x{2:X8} valid_slots={3}" -f
            $number, $snapshot.ControlValid, $snapshot.SessionId, $snapshot.ValidSlotCount)
        if ($snapshot.Latest) {
            $latest = $snapshot.Latest
            Write-Output ("  slot[{0}] seq={1} window_seq={2} samples={3} stft_frames={4}" -f
                $latest.Index, $latest.Sequence, $latest.WindowSequence,
                $latest.WindowSampleCount, $latest.StftFrameCount)
            Write-Output ("  STFT={0} cycles ({1} ms), NPU={2} cycles ({3} ms), E2E={4} cycles ({5} ms)" -f
                $latest.StftCycles, $latest.StftMs, $latest.NpuCycles, $latest.NpuMs,
                $latest.EndToEndCycles, $latest.EndToEndMs)
            Write-Output ("  timing_flags=0x{0:X} (STFT={1}, NPU={2}, E2E={3})" -f
                $latest.TimingFlags,
                (($latest.TimingFlags -band 1) -ne 0),
                (($latest.TimingFlags -band 2) -ne 0),
                (($latest.TimingFlags -band 4) -ne 0))
            Write-Output ("  inference_count={0} npu_ready={1} queue={2} drops={3}" -f
                $latest.NpuInferenceCount, $latest.NpuReady, $latest.QueueDepth, $latest.IngressDrops)
        }
        else { Write-Output '  latest display slot: none (control/session/seqlock not ready)' }
        Write-Output ("  tile_valid={0} tile_seq={1} tile_window_seq={2} tile_session=0x{3:X8} center={4} row={5}/{6} row_bytes={7}" -f
            $snapshot.Tile.Valid, $snapshot.Tile.Sequence, $snapshot.Tile.WindowSequence,
            $snapshot.Tile.SessionId, $snapshot.Tile.CenterIndex,
            $snapshot.Tile.NovelTimeStart, $snapshot.Tile.NovelTimeCount,
            $snapshot.Tile.RowBytes)
        Write-Output ("  runtime_valid={0} seq={1} heartbeat={2} stage={3} error={4} running={5} underflows={6}" -f
            $snapshot.Runtime.Valid, $snapshot.Runtime.Sequence, $snapshot.Runtime.Heartbeat,
            $snapshot.Runtime.Stage, $snapshot.Runtime.LastError, $snapshot.Runtime.Running,
            $snapshot.Runtime.Underflows)
        if ($snapshot.Runtime.MetricsVersion -ge 1) {
            Write-Output ("  board_rates: present={0} Hz content={1} Hz window={2} Hz inference={3} Hz tile={4} Hz underflow={5} Hz tick={6} ms frames={7}" -f
                $snapshot.Runtime.PresentedFpsHz, $snapshot.Runtime.ContentFpsHz,
                $snapshot.Runtime.WindowRateHz,
                $snapshot.Runtime.InferenceRateHz, $snapshot.Runtime.TileRateHz,
                $snapshot.Runtime.UnderflowRateHz, $snapshot.Runtime.LvglTickMs,
                $snapshot.Runtime.PresentedFrameCount)
        }
        if ($snapshot.Runtime.MetricsVersion -ge 3) {
            Write-Output ("  real_blocks: content_frames={0} waterfall_columns={1} tiles={2} drops={3}" -f
                $snapshot.Runtime.ContentFrameCount,
                $snapshot.Runtime.WaterfallColumnsGenerated,
                $snapshot.Runtime.WaterfallTilesConsumed,
                $snapshot.Runtime.WaterfallTilesDropped)
            Write-Output ("  ipc: frames={0} tiles={1} missed={2} session=0x{3:X8} frame_seq={4} tile_seq={5}" -f
                $snapshot.Runtime.IpcFramesReceived,
                $snapshot.Runtime.IpcTilesReceived,
                $snapshot.Runtime.IpcTilesMissed,
                $snapshot.Runtime.LastSessionId,
                $snapshot.Runtime.LastFrameSequence,
                $snapshot.Runtime.LastTileSequence)
            Write-Output ("  command: seq={0} status={1} reason={2} applied_session=0x{3:X8} flags=0x{4:X8} cpu0_ready={5} pending={6} live_retry={7}" -f
                $snapshot.Runtime.LastCommandSequence,
                $snapshot.Runtime.LastCommandStatus,
                $snapshot.Runtime.LastCommandReason,
                $snapshot.Runtime.LastAppliedSessionId,
                $snapshot.Runtime.RuntimeFlags,
                $snapshot.Runtime.Cpu0Ready,
                $snapshot.Runtime.CommandPending,
                $snapshot.Runtime.LiveRetryScheduled)
        }
        $number++
    }
    if ($Report.Interval) {
        $interval = $Report.Interval
        Write-Output ("Interval: measurement={0} s host_total={1} s session_changed={2}" -f
            $interval.MeasurementElapsedSeconds, $interval.HostElapsedSeconds, $interval.SessionChanged)
        Write-Output ("  heartbeat_delta={0} ({1} loop/s; liveness only)" -f
            $interval.HeartbeatDelta, $interval.HeartbeatRateHz)
        if (-not $interval.RuntimeAdvanced) {
            Write-Output '  WARNING: CPU1 runtime counters did not advance; underflow rate is not a live-display measurement.'
        }
        Write-Output ("  windows={0} ({1} Hz), STFT frames={2} Hz, inferences={3} ({4} Hz)" -f
            $interval.WindowDelta, $interval.WindowRateHz, $interval.StftFrameRateHz,
            $interval.InferenceDelta, $interval.InferenceRateHz)
        Write-Output ("  display_frames={0} ({1} Hz), tiles={2} ({3} Hz), underflows={4} ({5} Hz)" -f
            $interval.DisplayFrameDelta, $interval.DisplayFrameRateHz, $interval.TileDelta,
            $interval.TileRateHz, $interval.UnderflowDelta, $interval.UnderflowRateHz)
    }
}

Initialize-Layout
if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

$config = Read-HostConfig
$serial = Resolve-Probe $ProbeSerial $config
$jlink = Resolve-JLink $JLinkExe $config
$cpu0Path = Get-PreferredElf $Cpu0Elf 'CPU0'
$cpu1Path = Get-PreferredElf $Cpu1Elf 'CPU1'
$elfRecords = [ordered]@{
    Cpu0 = Get-ElfRecord $cpu0Path 'CPU0'
    Cpu1 = Get-ElfRecord $cpu1Path 'CPU1'
}

$commands = New-JLinkCommands $serial 0
$firstInvocation = Invoke-JLinkRead $jlink @($commands)
Assert-JLinkEvidence $firstInvocation.Text $firstInvocation.ExitCode $serial
$combinedText = $firstInvocation.Text
$hostElapsedSeconds = $firstInvocation.HostElapsedSeconds
if ($WindowSeconds -gt 0) {
    # A live CPU0 Commander session can keep CPU1 halted even after `go`. Close
    # the connection after resuming, let both cores run, then take snapshot 2.
    Start-Sleep -Seconds $WindowSeconds
    $secondInvocation = Invoke-JLinkRead $jlink @($commands)
    Assert-JLinkEvidence $secondInvocation.Text $secondInvocation.ExitCode $serial
    $combinedText += "`n" + $secondInvocation.Text
    $hostElapsedSeconds += $WindowSeconds + $secondInvocation.HostElapsedSeconds
}
$records = Get-Mem32Records $combinedText
$groups = Group-Mem32Occurrences $records
$snapshotCount = if ($WindowSeconds -gt 0) { 2 } else { 1 }
$snapshots = New-Object System.Collections.Generic.List[object]
for ($index = 0; $index -lt $snapshotCount; $index++) {
    $map = Select-Mem32Occurrence $groups $index
    [void] $snapshots.Add((Get-DisplaySnapshot $map))
}
$interval = if ($snapshotCount -eq 2) {
    Get-IntervalReport $snapshots[0] $snapshots[1] $hostElapsedSeconds $WindowSeconds
}
else { $null }

$report = [ordered]@{
    Tool = 'ra8p1-runtime-sampler'
    ToolVersion = '1.1'
    TimestampUtc = (Get-Date).ToUniversalTime().ToString('o')
    ProbeSerial = $serial
    Target = 'R7KA8P1KF_CPU0'
    JLinkExe = $jlink
    SnapshotCount = $snapshotCount
    RequestedWindowSeconds = $WindowSeconds
    Cpu0DwtClockHz = 1000000000
    CycleToMs = 'cycles / 1000000 (CPU0 DWT at 1 GHz)'
    Elf = $elfRecords
    Layout = $script:Layout
    Snapshots = @($snapshots | ForEach-Object { $_ })
    Interval = $interval
}

if ($Json) {
    $report | ConvertTo-Json -Depth 10
}
else {
    Write-HumanReport $report
}

<#
.SYNOPSIS
Reads CPU0 Ethernet, RMAC, IQ fast-path and IQ-ring counters over read-only SWD.

.DESCRIPTION
Symbol addresses and sizes are resolved from the exact CPU0 ELF with
arm-none-eabi-nm. J-Link Commander briefly halts CPU0, reads firmware-owned
accumulator objects, resumes CPU0 and exits. The script never loads or flashes
an image and never reads RMAC hardware counters directly.
#>
[CmdletBinding()]
param(
    [string] $ProbeSerial,
    [string] $JLinkExe,
    [string] $NmExe,
    [string] $Cpu0Elf,
    [ValidateRange(0, 3600)] [int] $WindowSeconds = 0,
    [switch] $Json,
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$layoutHelper = Join-Path $PSScriptRoot 'project-layout.ps1'
. $layoutHelper
$script:Cpu0Project = (Resolve-Ra8p1ProjectLayout -Solution $script:ProjectRoot).Cpu0Directory
$script:ExpectedSymbols = [ordered]@{
    Eth  = [pscustomobject]@{ Name = 'g_eth_diag'; MinimumBytes = 396 }
    Rmac = [pscustomobject]@{ Name = 'g_eth_rmac_diag'; MinimumBytes = 100 }
    Iq   = [pscustomobject]@{ Name = 'g_eth_iq_fast_stats'; MinimumBytes = 128 }
    Ring = [pscustomobject]@{ Name = 'g_iq_ring_control'; MinimumBytes = 28 }
    Perf = [pscustomobject]@{ Name = 'g_sdr_iiod_perf_result'; MinimumBytes = 208 }
    Control = [pscustomobject]@{ Name = 'g_sdr_control_stats'; MinimumBytes = 152 }
}

function Format-Hex32 {
    param([uint32] $Value)
    return ('0x{0:X8}' -f $Value)
}

function Add-Address {
    param([uint32] $Base, [int] $Offset)
    return [uint32] ([uint64] $Base + [uint64] $Offset)
}

function Read-HostConfig {
    $path = Join-Path $HOME '.codex\ra8p1.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return @{} }
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
    [void] $candidates.Add((Join-Path $HOME 'SEGGER\JLink_V958\JLink.exe'))
    [void] $candidates.Add('C:\Program Files\SEGGER\JLink\JLink.exe')
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'JLink.exe was not found. Pass -JLinkExe or set RA8P1_JLINK_ROOT.'
}

function Resolve-Nm {
    param([string] $Requested, [hashtable] $Config)
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($Requested) { [void] $candidates.Add($Requested) }
    if ($env:RA8P1_E2_ROOT) {
        [void] $candidates.Add((Join-Path $env:RA8P1_E2_ROOT 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe'))
    }
    if ($Config['E2Root']) {
        [void] $candidates.Add((Join-Path $Config['E2Root'] 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe'))
    }
    $command = Get-Command arm-none-eabi-nm.exe -ErrorAction SilentlyContinue
    if ($command) { [void] $candidates.Add($command.Source) }
    [void] $candidates.Add('C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe')
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'arm-none-eabi-nm.exe was not found. Pass -NmExe or set RA8P1_E2_ROOT.'
}

function Resolve-Cpu0Elf {
    param([string] $Requested)
    if ($Requested) {
        $path = [IO.Path]::GetFullPath($Requested)
    }
    else {
        $debug = Join-Path $script:Cpu0Project 'Debug'
        $file = Get-ChildItem -LiteralPath $debug -Filter '*.elf' -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notmatch '\.before-' } |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if (-not $file) { throw "CPU0 ELF was not found under $debug." }
        $path = $file.FullName
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "CPU0 ELF does not exist: $path"
    }
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-ElfRecord {
    param([Parameter(Mandatory)] [string] $Path)
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        Path = $item.FullName
        Size = [uint64] $item.Length
        LastWriteTimeUtc = $item.LastWriteTimeUtc.ToString('o')
        Sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
    }
}

function Get-ElfSymbols {
    param(
        [Parameter(Mandatory)] [string] $Nm,
        [Parameter(Mandatory)] [string] $Elf
    )
    $lines = @(& $Nm -S -n $Elf 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "arm-none-eabi-nm failed with exit code $LASTEXITCODE.`n$($lines -join "`n")"
    }
    $found = @{}
    foreach ($line in $lines) {
        $match = [regex]::Match([string] $line,
            '^\s*(?<address>[0-9A-Fa-f]+)\s+(?<size>[0-9A-Fa-f]+)\s+(?<type>\S)\s+(?<name>\S+)\s*$')
        if (-not $match.Success) { continue }
        $name = $match.Groups['name'].Value
        foreach ($entry in $script:ExpectedSymbols.Values) {
            if ($name -eq $entry.Name) {
                $found[$name] = [pscustomobject]@{
                    Name = $name
                    Address = [uint32] ([Convert]::ToUInt64($match.Groups['address'].Value, 16))
                    Size = [uint32] ([Convert]::ToUInt64($match.Groups['size'].Value, 16))
                    Type = $match.Groups['type'].Value
                }
            }
        }
    }
    $result = [ordered]@{}
    foreach ($key in $script:ExpectedSymbols.Keys) {
        $expected = $script:ExpectedSymbols[$key]
        if (-not $found.ContainsKey($expected.Name)) {
            throw "Required ELF symbol was not found: $($expected.Name)"
        }
        $symbol = $found[$expected.Name]
        if ($symbol.Size -lt $expected.MinimumBytes) {
            throw "ELF symbol $($symbol.Name) is $($symbol.Size) bytes; expected at least $($expected.MinimumBytes)."
        }
        $result[$key] = $symbol
    }
    return $result
}

function Get-Mem32Map {
    param([Parameter(Mandatory)] [string] $Text)
    $map = @{}
    foreach ($line in ($Text -split "`r?`n")) {
        $match = [regex]::Match($line, '^\s*(?:J-Link>\s*)?([0-9A-Fa-f]{8})\s*=\s*(.+)$')
        if (-not $match.Success) { continue }
        $start = [uint32] ([Convert]::ToUInt64($match.Groups[1].Value, 16))
        $words = [regex]::Matches($match.Groups[2].Value,
                                  '(?<![0-9A-Fa-f])([0-9A-Fa-f]{8})(?![0-9A-Fa-f])')
        $index = 0
        foreach ($word in $words) {
            $address = Add-Address $start (4 * $index)
            $map[('{0:X8}' -f $address)] =
                [uint32] ([Convert]::ToUInt64($word.Groups[1].Value, 16))
            $index++
        }
    }
    return $map
}

function Get-U32 {
    param([hashtable] $Map, [uint32] $Base, [int] $Offset)
    $address = Add-Address $Base $Offset
    $key = '{0:X8}' -f $address
    if (-not $Map.ContainsKey($key)) {
        throw "Memory word $(Format-Hex32 $address) was absent from J-Link output."
    }
    return [uint32] $Map[$key]
}

function Get-U64 {
    param([hashtable] $Map, [uint32] $Base, [int] $Offset)
    $low = [uint64] (Get-U32 $Map $Base $Offset)
    $high = [uint64] (Get-U32 $Map $Base ($Offset + 4))
    return [uint64] ($low -bor ($high -shl 32))
}

function Get-I32 {
    param([hashtable] $Map, [uint32] $Base, [int] $Offset)
    $bytes = [BitConverter]::GetBytes((Get-U32 $Map $Base $Offset))
    return [BitConverter]::ToInt32($bytes, 0)
}

function Get-Delta32 {
    param([uint32] $NewValue, [uint32] $OldValue)
    $new64 = [uint64] $NewValue
    $old64 = [uint64] $OldValue
    if ($new64 -ge $old64) { return [uint64] ($new64 - $old64) }
    return [uint64] ([uint64]4294967296 + $new64 - $old64)
}

function Get-Delta64 {
    param([uint64] $NewValue, [uint64] $OldValue)
    if ($NewValue -ge $OldValue) { return [uint64] ($NewValue - $OldValue) }
    return $null
}

function Get-NetSnapshot {
    param(
        [Parameter(Mandatory)] [hashtable] $Map,
        [Parameter(Mandatory)] $Symbols
    )
    $ethBase = [uint32] $Symbols.Eth.Address
    $rmacBase = [uint32] $Symbols.Rmac.Address
    $iqBase = [uint32] $Symbols.Iq.Address
    $ringBase = [uint32] $Symbols.Ring.Address
    $perfBase = [uint32] $Symbols.Perf.Address
    $controlBase = [uint32] $Symbols.Control.Address

    $ethMagic = Get-U32 $Map $ethBase 0
    $rmacMagic = Get-U32 $Map $rmacBase 0
    $iqMagic = Get-U32 $Map $iqBase 0
    $iqSchema = Get-U32 $Map $iqBase 4
    $iqInitialized = ($iqMagic -eq 0x5149504B)
    $perfMagic = Get-U32 $Map $perfBase 0
    $perfSchema = Get-U32 $Map $perfBase 4
    $perfInitialized = (($perfMagic -eq 0x46524453) -and ($perfSchema -eq 2))
    if ($ethMagic -ne 0x45444847) { throw "g_eth_diag magic mismatch: $(Format-Hex32 $ethMagic)." }
    if ($rmacMagic -ne 0x524D4143) { throw "g_eth_rmac_diag magic mismatch: $(Format-Hex32 $rmacMagic)." }
    if (-not $iqInitialized -and (($iqMagic -ne 0) -or ($iqSchema -ne 0))) {
        throw "g_eth_iq_fast_stats header mismatch: magic=$(Format-Hex32 $iqMagic), schema=$iqSchema."
    }
    if (-not $perfInitialized -and (($perfMagic -ne 0) -or ($perfSchema -ne 0))) {
        throw "g_sdr_iiod_perf_result header mismatch: magic=$(Format-Hex32 $perfMagic), schema=$perfSchema."
    }

    $mdioMask = Get-U32 $Map $ethBase 96
    $mdio = New-Object System.Collections.Generic.List[object]
    for ($address = 0; $address -lt 32; $address++) {
        if (($mdioMask -band ([uint32]1 -shl $address)) -ne 0) {
            [void] $mdio.Add([pscustomobject]@{
                Address = $address
                Id1 = Get-U32 $Map $ethBase (100 + (4 * $address))
                Id2 = Get-U32 $Map $ethBase (228 + (4 * $address))
            })
        }
    }

    $head = Get-U32 $Map $ringBase 0
    $tail = Get-U32 $Map $ringBase 4
    $queued = Get-Delta32 $head $tail
    return [pscustomobject]@{
        TimestampUtc = (Get-Date).ToUniversalTime().ToString('o')
        Phy = [pscustomobject]@{
            OpenResult = Get-U32 $Map $ethBase 4
            ReadOk = Get-U32 $Map $ethBase 8
            ReadFail = Get-U32 $Map $ethBase 12
            LinkStatusMask = Get-U32 $Map $ethBase 16
            LinkUp = ((Get-U32 $Map $ethBase 16) -ne 0)
            LinkProcessOk = Get-U32 $Map $ethBase 20
            LinkProcessFail = Get-U32 $Map $ethBase 24
            IrqLinkOn = Get-U32 $Map $ethBase 28
            IrqLinkOff = Get-U32 $Map $ethBase 32
            MdioFoundMask = $mdioMask
            MdioDevices = @($mdio | ForEach-Object { $_ })
            Anar = Get-U32 $Map $rmacBase 12
            Anlpar = Get-U32 $Map $rmacBase 16
            GigabitControl = Get-U32 $Map $rmacBase 20
            GigabitStatus = Get-U32 $Map $rmacBase 24
        }
        Rmac = [pscustomobject]@{
            Version = Get-U32 $Map $rmacBase 4
            Snapshots = Get-U32 $Map $rmacBase 8
            MacAutoPauseTx = Get-U32 $Map $rmacBase 28
            MacPauseRx = Get-U32 $Map $rmacBase 32
            RxOverflow = Get-U32 $Map $rmacBase 36
            RxGoodEFrames = Get-U32 $Map $rmacBase 40
            RxGoodPFrames = Get-U32 $Map $rmacBase 44
            RxBroadcastFrames = Get-U32 $Map $rmacBase 48
            RxMulticastFrames = Get-U32 $Map $rmacBase 52
            RxUnicastFrames = Get-U32 $Map $rmacBase 56
            RxErrorFrames = Get-U32 $Map $rmacBase 60
            RxAllFrames = Get-U32 $Map $rmacBase 64
            RxGoodUndersize = Get-U32 $Map $rmacBase 68
            RxBadUndersize = Get-U32 $Map $rmacBase 72
            RxGoodOversize = Get-U32 $Map $rmacBase 76
            RxBadOversize = Get-U32 $Map $rmacBase 80
            RxFcsErrorRaw = Get-U32 $Map $rmacBase 84
            RxFragmentErrorRaw = Get-U32 $Map $rmacBase 88
            RxMessageLostIrq = Get-U32 $Map $rmacBase 92
            GlobalErrorIrq = Get-U32 $Map $rmacBase 96
            DriverRxCalls = Get-U32 $Map $ethBase 68
            DriverRxOk = Get-U32 $Map $ethBase 72
            DriverRxEmpty = Get-U32 $Map $ethBase 76
            DriverRxFail = Get-U32 $Map $ethBase 80
            DriverRxPbufAllocFail = Get-U32 $Map $ethBase 84
            DriverRxLastLength = Get-U32 $Map $ethBase 88
            DriverRxLastResult = Get-U32 $Map $ethBase 92
            LwipTcpipInpktAllocFail = Get-U32 $Map $ethBase 388
            LwipTcpipInpktMboxFail = Get-U32 $Map $ethBase 392
            IrqRxComplete = Get-U32 $Map $ethBase 36
            IrqRxMessageLost = Get-U32 $Map $ethBase 40
            IrqErrorGlobal = Get-U32 $Map $ethBase 44
        }
        IiodPerf = [pscustomobject]@{
            Initialized = $perfInitialized
            Magic = $perfMagic
            SchemaVersion = $perfSchema
            State = Get-U32 $Map $perfBase 8
            LastError = Get-I32 $Map $perfBase 12
            RequestBytes = Get-U32 $Map $perfBase 100
            ReadRequests = Get-U32 $Map $perfBase 104
            ReadChunks = Get-U32 $Map $perfBase 108
            StreamMask = Get-U32 $Map $perfBase 112
            ElapsedMs = Get-U32 $Map $perfBase 116
            PayloadMbpsX1000 = Get-U32 $Map $perfBase 120
            Checksum = Get-U32 $Map $perfBase 124
            Errors = Get-U32 $Map $perfBase 128
            BytesReceived = Get-U64 $Map $perfBase 136
            TargetBytes = Get-U64 $Map $perfBase 184
            RcvbufSetsockoptResult = Get-I32 $Map $perfBase 192
            RecvCalls = Get-U32 $Map $perfBase 196
            CacheFills = Get-U32 $Map $perfBase 200
        }
        Iq = [pscustomobject]@{
            Initialized = $iqInitialized
            SchemaVersion = $iqSchema
            Active = Get-U32 $Map $iqBase 8
            Packets = Get-U32 $Map $iqBase 12
            PayloadBytes = Get-U64 $Map $iqBase 16
            SequenceGaps = Get-U32 $Map $iqBase 24
            Reordered = Get-U32 $Map $iqBase 28
            Invalid = Get-U32 $Map $iqBase 32
            FirstTick = Get-U32 $Map $iqBase 36
            LastTick = Get-U32 $Map $iqBase 40
            ElapsedMs = Get-U32 $Map $iqBase 44
            MbpsX1000 = Get-U32 $Map $iqBase 48
            NextSequence = Get-U32 $Map $iqBase 52
            DataChecksum = Get-U32 $Map $iqBase 56
            SessionId = Get-U32 $Map $iqBase 60
            Flags = Get-U32 $Map $iqBase 64
            Crc32c = Get-U32 $Map $iqBase 68
            ExpectedCrc32c = Get-U32 $Map $iqBase 72
            CrcErrors = Get-U32 $Map $iqBase 76
            CrcFlags = Get-U32 $Map $iqBase 80
            CrcBackend = Get-U32 $Map $iqBase 84
            CrcUpdates = Get-U32 $Map $iqBase 88
            CrcCyclesTotal = Get-U64 $Map $iqBase 96
            CrcCyclesMax = Get-U32 $Map $iqBase 104
            CrcHardwareSelfTest = Get-U32 $Map $iqBase 108
            EndPacketCpu0Cycles = Get-U32 $Map $iqBase 112
            CrcCompleteCpu0Cycles = Get-U32 $Map $iqBase 116
            CrcAfterEndCycles = Get-U32 $Map $iqBase 120
            CrcTimingFlags = Get-U32 $Map $iqBase 124
        }
        SdrControl = [pscustomobject]@{
            State = Get-U32 $Map $controlBase 0
            RequestId = Get-U32 $Map $controlBase 4
            SessionId = Get-U32 $Map $controlBase 8
            CenterIndex = Get-U32 $Map $controlBase 12
            CompletedWindows = Get-U32 $Map $controlBase 16
            TxDatagrams = Get-U32 $Map $controlBase 20
            RxDatagrams = Get-U32 $Map $controlBase 24
            InvalidDatagrams = Get-U32 $Map $controlBase 28
            Retries = Get-U32 $Map $controlBase 32
            Timeouts = Get-U32 $Map $controlBase 36
            LastStatus = Get-U32 $Map $controlBase 40
            LastTxMs = Get-U32 $Map $controlBase 44
            LastRxMs = Get-U32 $Map $controlBase 48
            RequestStartMs = Get-U32 $Map $controlBase 52
            RequestElapsedMs = Get-U32 $Map $controlBase 56
            ActualPayloadMbpsX1000 = Get-U32 $Map $controlBase 60
            WindowCrc32c = Get-U32 $Map $controlBase 64
            LastMessageCrc32c = Get-U32 $Map $controlBase 68
            AgentRequestRxUs = Get-U64 $Map $controlBase 72
            TuneStartUs = Get-U64 $Map $controlBase 80
            TuneCompleteUs = Get-U64 $Map $controlBase 88
            CaptureStartUs = Get-U64 $Map $controlBase 96
            CaptureCompleteUs = Get-U64 $Map $controlBase 104
            BootEpoch = Get-U64 $Map $controlBase 112
            PrefetchedRequestId = Get-U32 $Map $controlBase 120
            PrefetchedSessionId = Get-U32 $Map $controlBase 124
            PrefetchedCenterIndex = Get-U32 $Map $controlBase 128
            PrefetchState = Get-U32 $Map $controlBase 132
            PrefetchedWindows = Get-U32 $Map $controlBase 136
            MissingCaptureComplete = Get-U32 $Map $controlBase 140
            PrefetchCreditWithoutReady = Get-U32 $Map $controlBase 144
            PrefetchIqscCreditProofs = Get-U32 $Map $controlBase 148
        }
        Ring = [pscustomobject]@{
            Capacity = 4096
            Head = $head
            Tail = $tail
            Pushed = Get-U32 $Map $ringBase 8
            Popped = Get-U32 $Map $ringBase 12
            FullDrops = Get-U32 $Map $ringBase 16
            OversizeDrops = Get-U32 $Map $ringBase 20
            HighWatermark = Get-U32 $Map $ringBase 24
            Queued = $queued
        }
    }
}

function New-JLinkCommands {
    param(
        [Parameter(Mandatory)] $Symbols
    )
    $commands = New-Object System.Collections.Generic.List[string]
    # Windows PowerShell 5 + J-Link v9.56 can occasionally consume the first
    # stdin line as an empty/unknown command.  Duplicate an idempotent speed
    # command so `halt` is never the sacrificial first line.
    foreach ($command in @('speed 4000', 'speed 4000', 'halt')) { [void] $commands.Add($command) }
    foreach ($key in $script:ExpectedSymbols.Keys) {
        $symbol = $Symbols[$key]
        $words = [math]::Ceiling([double] $symbol.Size / 4.0)
        [void] $commands.Add(("mem32 {0} {1}" -f (Format-Hex32 $symbol.Address), $words))
    }
    [void] $commands.Add('go')
    [void] $commands.Add('exit')
    return $commands
}

function Invoke-JLinkRead {
    param(
        [Parameter(Mandatory)] [string] $Executable,
        [Parameter(Mandatory)] [string[]] $Commands,
        [Parameter(Mandatory)] [string] $Serial
    )
    $inputText = ($Commands -join "`r`n") + "`r`n"
    $lines = New-Object System.Collections.Generic.List[string]
    $saved = $ErrorActionPreference
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $arguments = @(
        '-NoGui', '1',
        '-SelectEmuBySN', $Serial,
        '-Device', 'R7KA8P1KF_CPU0',
        '-If', 'SWD',
        '-Speed', '4000',
        '-AutoConnect', '1'
    )
    try {
        $ErrorActionPreference = 'Continue'
        (($inputText | & $Executable @arguments 2>&1) | ForEach-Object {
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
    if ($Text -match 'Cannot connect|Failed to connect|Could not find emulator|No emulator connected|Target connection not established|CPU is not halted') {
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
    $haltIndex = $Text.IndexOf('PC =', [StringComparison]::OrdinalIgnoreCase)
    foreach ($match in [regex]::Matches($Text, '(?im)^.*Unknown command.*$')) {
        if (($haltIndex -lt 0) -or ($match.Index -gt $haltIndex)) {
            throw "J-Link command failed after CPU0 halt.`n$Text"
        }
    }
}

function Read-LiveSnapshot {
    param(
        [Parameter(Mandatory)] [string] $JLink,
        [Parameter(Mandatory)] [string] $Serial,
        [Parameter(Mandatory)] $Symbols
    )
    $invocation = Invoke-JLinkRead $JLink (New-JLinkCommands $Symbols) $Serial
    Assert-JLinkEvidence $invocation.Text $invocation.ExitCode $Serial
    return [pscustomobject]@{
        Snapshot = Get-NetSnapshot (Get-Mem32Map $invocation.Text) $Symbols
        HostElapsedSeconds = $invocation.HostElapsedSeconds
    }
}

function Get-Interval {
    param($First, $Last, [double] $Seconds)
    if ($Seconds -le 0) { return $null }
    $payload = Get-Delta64 $Last.Iq.PayloadBytes $First.Iq.PayloadBytes
    return [pscustomobject]@{
        Seconds = $Seconds
        SessionChanged = ($First.Iq.SessionId -ne $Last.Iq.SessionId)
        RmacSnapshotDelta = Get-Delta32 $Last.Rmac.Snapshots $First.Rmac.Snapshots
        RmacRxAllFrameDelta = Get-Delta32 $Last.Rmac.RxAllFrames $First.Rmac.RxAllFrames
        DriverRxOkDelta = Get-Delta32 $Last.Rmac.DriverRxOk $First.Rmac.DriverRxOk
        LwipTcpipInpktAllocFailDelta = Get-Delta32 $Last.Rmac.LwipTcpipInpktAllocFail $First.Rmac.LwipTcpipInpktAllocFail
        LwipTcpipInpktMboxFailDelta = Get-Delta32 $Last.Rmac.LwipTcpipInpktMboxFail $First.Rmac.LwipTcpipInpktMboxFail
        IqPacketDelta = Get-Delta32 $Last.Iq.Packets $First.Iq.Packets
        IqPayloadByteDelta = $payload
        IqSequenceGapDelta = Get-Delta32 $Last.Iq.SequenceGaps $First.Iq.SequenceGaps
        IqReorderedDelta = Get-Delta32 $Last.Iq.Reordered $First.Iq.Reordered
        IqInvalidDelta = Get-Delta32 $Last.Iq.Invalid $First.Iq.Invalid
        IqCrcErrorDelta = Get-Delta32 $Last.Iq.CrcErrors $First.Iq.CrcErrors
        SdrControlCompletedWindowDelta = Get-Delta32 $Last.SdrControl.CompletedWindows $First.SdrControl.CompletedWindows
        SdrControlTxDelta = Get-Delta32 $Last.SdrControl.TxDatagrams $First.SdrControl.TxDatagrams
        SdrControlRxDelta = Get-Delta32 $Last.SdrControl.RxDatagrams $First.SdrControl.RxDatagrams
        SdrControlRetryDelta = Get-Delta32 $Last.SdrControl.Retries $First.SdrControl.Retries
        SdrControlTimeoutDelta = Get-Delta32 $Last.SdrControl.Timeouts $First.SdrControl.Timeouts
        SdrControlMissingCaptureCompleteDelta = Get-Delta32 $Last.SdrControl.MissingCaptureComplete $First.SdrControl.MissingCaptureComplete
        SdrControlPrefetchCreditWithoutReadyDelta = Get-Delta32 $Last.SdrControl.PrefetchCreditWithoutReady $First.SdrControl.PrefetchCreditWithoutReady
        SdrControlPrefetchIqscCreditProofDelta = Get-Delta32 $Last.SdrControl.PrefetchIqscCreditProofs $First.SdrControl.PrefetchIqscCreditProofs
        RingFullDropDelta = Get-Delta32 $Last.Ring.FullDrops $First.Ring.FullDrops
        RingOversizeDropDelta = Get-Delta32 $Last.Ring.OversizeDrops $First.Ring.OversizeDrops
        IqPacketRate = [math]::Round(([double](Get-Delta32 $Last.Iq.Packets $First.Iq.Packets) / $Seconds), 3)
        IqPayloadMbps = if ($null -eq $payload) { $null } else {
            [math]::Round(([double]$payload * 8.0 / $Seconds / 1000000.0), 3)
        }
    }
}

function Set-FakeWord {
    param([hashtable] $Map, [uint32] $Address, [uint32] $Value)
    $Map[('{0:X8}' -f $Address)] = $Value
}

function Set-FakeU64 {
    param([hashtable] $Map, [uint32] $Address, [uint64] $Value)
    Set-FakeWord $Map $Address ([uint32] ($Value -band 0xFFFFFFFFL))
    Set-FakeWord $Map (Add-Address $Address 4) ([uint32] ($Value -shr 32))
}

function New-FakeMem32Text {
    param([hashtable] $Map)
    $lines = New-Object System.Collections.Generic.List[string]
    $addresses = @($Map.Keys | ForEach-Object { [uint32] ([Convert]::ToUInt64($_, 16)) } | Sort-Object)
    foreach ($address in $addresses) {
        $key = '{0:X8}' -f $address
        [void] $lines.Add(('{0:X8} = {1:X8}' -f $address, [uint32]$Map[$key]))
    }
    return ($lines -join "`n")
}

function Invoke-SelfTest {
    $crcVector = [uint32]3808858755 # CRC32C("123456789") = 0xE3069283
    $targetBytes = [uint64]11806720
    $requestBytes = [uint64]1048576
    $requestCount = [uint64][math]::Ceiling([double]$targetBytes / [double]$requestBytes)
    $lastRequestBytes = $targetBytes - (($requestCount - 1) * $requestBytes)
    if ($requestCount -ne 12 -or $lastRequestBytes -ne 272384) {
        throw 'Self-test failed: fixed-byte READBUF schedule.'
    }

    $symbols = [ordered]@{
        Eth  = [pscustomobject]@{ Name = 'g_eth_diag'; Address = [uint32]0x22001000; Size = 396; Type = 'D' }
        Rmac = [pscustomobject]@{ Name = 'g_eth_rmac_diag'; Address = [uint32]0x22002000; Size = 100; Type = 'D' }
        Iq   = [pscustomobject]@{ Name = 'g_eth_iq_fast_stats'; Address = [uint32]0x22003000; Size = 128; Type = 'B' }
        Ring = [pscustomobject]@{ Name = 'g_iq_ring_control'; Address = [uint32]0x22004000; Size = 28; Type = 'b' }
        Perf = [pscustomobject]@{ Name = 'g_sdr_iiod_perf_result'; Address = [uint32]0x22005000; Size = 208; Type = 'D' }
        Control = [pscustomobject]@{ Name = 'g_sdr_control_stats'; Address = [uint32]0x22006000; Size = 152; Type = 'B' }
    }
    $memory = @{}
    foreach ($symbol in $symbols.Values) {
        for ($offset = 0; $offset -lt $symbol.Size; $offset += 4) {
            Set-FakeWord $memory (Add-Address $symbol.Address $offset) 0
        }
    }
    Set-FakeWord $memory $symbols.Eth.Address 0x45444847
    Set-FakeWord $memory (Add-Address $symbols.Eth.Address 16) 1
    Set-FakeWord $memory (Add-Address $symbols.Eth.Address 68) 1200
    Set-FakeWord $memory (Add-Address $symbols.Eth.Address 72) 1100
    Set-FakeWord $memory (Add-Address $symbols.Eth.Address 96) 2
    Set-FakeWord $memory (Add-Address $symbols.Eth.Address 104) 0x001C
    Set-FakeWord $memory (Add-Address $symbols.Eth.Address 232) 0xC916
    Set-FakeWord $memory (Add-Address $symbols.Eth.Address 388) 3
    Set-FakeWord $memory (Add-Address $symbols.Eth.Address 392) 4
    Set-FakeWord $memory $symbols.Rmac.Address 0x524D4143
    Set-FakeWord $memory (Add-Address $symbols.Rmac.Address 4) 1
    Set-FakeWord $memory (Add-Address $symbols.Rmac.Address 8) 9
    Set-FakeWord $memory (Add-Address $symbols.Rmac.Address 64) 1000
    Set-FakeWord $memory $symbols.Iq.Address 0x5149504B
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 4) 6
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 8) 1
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 12) 777
    Set-FakeU64 $memory (Add-Address $symbols.Iq.Address 16) ([uint64]123456789)
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 24) 4
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 28) 2
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 32) 3
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 60) 42
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 68) $crcVector
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 72) $crcVector
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 76) 0
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 80) 0x0C
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 84) 2
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 88) 777
    Set-FakeU64 $memory (Add-Address $symbols.Iq.Address 96) ([uint64]1234567)
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 104) 4321
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 108) 1
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 112) 2000000
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 116) 2001234
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 120) 1234
    Set-FakeWord $memory (Add-Address $symbols.Iq.Address 124) 3
    Set-FakeWord $memory $symbols.Ring.Address 105
    Set-FakeWord $memory (Add-Address $symbols.Ring.Address 4) 100
    Set-FakeWord $memory (Add-Address $symbols.Ring.Address 8) 105
    Set-FakeWord $memory (Add-Address $symbols.Ring.Address 12) 100
    Set-FakeWord $memory (Add-Address $symbols.Ring.Address 16) 6
    Set-FakeWord $memory (Add-Address $symbols.Ring.Address 20) 7
    Set-FakeWord $memory (Add-Address $symbols.Ring.Address 24) 512
    Set-FakeWord $memory $symbols.Perf.Address 0x46524453
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 4) 2
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 8) 7
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 12) 0
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 100) 1048576
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 104) 12
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 108) 144
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 116) 1000
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 120) 800000
    Set-FakeU64 $memory (Add-Address $symbols.Perf.Address 136) ([uint64]11806720)
    Set-FakeU64 $memory (Add-Address $symbols.Perf.Address 184) ([uint64]11806720)
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 192) 0
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 196) 24
    Set-FakeWord $memory (Add-Address $symbols.Perf.Address 200) 24
    Set-FakeWord $memory $symbols.Control.Address 5
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 4) 101
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 8) 201
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 12) 2
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 16) 3
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 20) 7
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 24) 6
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 32) 1
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 36) 2
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 40) 0
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 60) 390000
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 64) $crcVector
    Set-FakeU64 $memory (Add-Address $symbols.Control.Address 72) ([uint64]1000000)
    Set-FakeU64 $memory (Add-Address $symbols.Control.Address 80) ([uint64]1000100)
    Set-FakeU64 $memory (Add-Address $symbols.Control.Address 88) ([uint64]1000200)
    Set-FakeU64 $memory (Add-Address $symbols.Control.Address 96) ([uint64]1000300)
    Set-FakeU64 $memory (Add-Address $symbols.Control.Address 104) ([uint64]1010138)
    Set-FakeU64 $memory (Add-Address $symbols.Control.Address 112) ([uint64]0x123456789ABCDEF0)
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 120) 102
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 124) 202
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 128) 3
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 132) 4
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 136) 2
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 140) 5
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 144) 6
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 148) 7
    $snapshot = Get-NetSnapshot (Get-Mem32Map (New-FakeMem32Text $memory)) $symbols
    if (-not $snapshot.Phy.LinkUp) { throw 'Self-test failed: PHY link.' }
    if ($snapshot.Rmac.Snapshots -ne 9 -or $snapshot.Rmac.RxAllFrames -ne 1000) {
        throw 'Self-test failed: RMAC counters.'
    }
    if ($snapshot.Rmac.LwipTcpipInpktAllocFail -ne 3 -or
        $snapshot.Rmac.LwipTcpipInpktMboxFail -ne 4) {
        throw 'Self-test failed: lwIP tcpip input failure counters.'
    }
    if ($snapshot.IiodPerf.SchemaVersion -ne 2 -or
        $snapshot.IiodPerf.TargetBytes -ne 11806720 -or
        $snapshot.IiodPerf.BytesReceived -ne 11806720 -or
        $snapshot.IiodPerf.ReadRequests -ne 12 -or
        $snapshot.IiodPerf.RcvbufSetsockoptResult -ne 0) {
        throw 'Self-test failed: iiod fixed-byte result ABI.'
    }
    if ($snapshot.Iq.SchemaVersion -ne 6 -or
        $snapshot.Iq.Packets -ne 777 -or $snapshot.Iq.PayloadBytes -ne 123456789 -or
        $snapshot.Iq.SequenceGaps -ne 4 -or $snapshot.Iq.Reordered -ne 2 -or
        $snapshot.Iq.Invalid -ne 3 -or $snapshot.Iq.Crc32c -ne $crcVector -or
        $snapshot.Iq.ExpectedCrc32c -ne $crcVector -or $snapshot.Iq.CrcErrors -ne 0 -or
        $snapshot.Iq.CrcFlags -ne 0x0C -or $snapshot.Iq.CrcBackend -ne 2 -or
        $snapshot.Iq.CrcUpdates -ne 777 -or $snapshot.Iq.CrcCyclesTotal -ne 1234567 -or
        $snapshot.Iq.CrcCyclesMax -ne 4321 -or $snapshot.Iq.CrcHardwareSelfTest -ne 1 -or
        $snapshot.Iq.EndPacketCpu0Cycles -ne 2000000 -or
        $snapshot.Iq.CrcCompleteCpu0Cycles -ne 2001234 -or
        $snapshot.Iq.CrcAfterEndCycles -ne 1234 -or $snapshot.Iq.CrcTimingFlags -ne 3) {
        throw 'Self-test failed: IQ counters.'
    }
    if ($snapshot.Ring.Queued -ne 5 -or $snapshot.Ring.FullDrops -ne 6 -or
        $snapshot.Ring.OversizeDrops -ne 7 -or $snapshot.Ring.HighWatermark -ne 512) {
        throw 'Self-test failed: ring counters.'
    }
    if ($snapshot.SdrControl.RequestId -ne 101 -or
        $snapshot.SdrControl.SessionId -ne 201 -or
        $snapshot.SdrControl.CenterIndex -ne 2 -or
        $snapshot.SdrControl.CaptureCompleteUs -ne 1010138 -or
        $snapshot.SdrControl.BootEpoch -ne [uint64]0x123456789ABCDEF0 -or
        $snapshot.SdrControl.MissingCaptureComplete -ne 5 -or
        $snapshot.SdrControl.PrefetchCreditWithoutReady -ne 6 -or
        $snapshot.SdrControl.PrefetchIqscCreditProofs -ne 7) {
        throw 'Self-test failed: SDR control field decoding.'
    }
    $jsonSnapshot = $snapshot | ConvertTo-Json -Depth 9 | ConvertFrom-Json
    if ($jsonSnapshot.SdrControl.PrefetchIqscCreditProofs -ne 7) {
        throw 'Self-test failed: SDR control JSON field serialization.'
    }
    Set-FakeWord $memory (Add-Address $symbols.Control.Address 148) 9
    $nextSnapshot = Get-NetSnapshot (Get-Mem32Map (New-FakeMem32Text $memory)) $symbols
    $interval = Get-Interval $snapshot $nextSnapshot 1.0
    if ($interval.SdrControlPrefetchIqscCreditProofDelta -ne 2) {
        throw 'Self-test failed: SDR control IQSC credit proof delta.'
    }
    Write-Output 'Self-test passed: mem32 parser, magic validation, PHY/RMAC/lwIP/IQ/CRC/ring/iiod/SDRC field decoding.'
}

function Write-HumanReport {
    param([Parameter(Mandatory)] $Report)
    Write-Output 'RA8P1 CPU0 network statistics (read-only SWD; firmware accumulators)'
    Write-Output ("Probe={0} target={1}" -f $Report.ProbeSerial, $Report.Target)
    Write-Output ("ELF={0}" -f $Report.Elf.Path)
    Write-Output ("ELF_SHA256={0} size={1} timestamp_utc={2}" -f
        $Report.Elf.Sha256, $Report.Elf.Size, $Report.Elf.LastWriteTimeUtc)
    Write-Output 'NOTE: the ELF hash binds symbol interpretation; it does not prove the board was flashed with this ELF.'
    Write-Output 'WARNING: halt-based SWD is read-only but can perturb a live high-rate Ethernet stream.'
    $index = 1
    foreach ($snapshot in $Report.Snapshots) {
        Write-Output ("Snapshot {0} @ {1}" -f $index, $snapshot.TimestampUtc)
        Write-Output ("  PHY: link_up={0} mask=0x{1:X} open={2} read_ok={3} read_fail={4} mdio_mask=0x{5:X8}" -f
            $snapshot.Phy.LinkUp, $snapshot.Phy.LinkStatusMask, $snapshot.Phy.OpenResult,
            $snapshot.Phy.ReadOk, $snapshot.Phy.ReadFail, $snapshot.Phy.MdioFoundMask)
        Write-Output ("  PHY raw: ANAR=0x{0:X4} ANLPAR=0x{1:X4} GBCR=0x{2:X4} GBSR=0x{3:X4}" -f
            $snapshot.Phy.Anar, $snapshot.Phy.Anlpar,
            $snapshot.Phy.GigabitControl, $snapshot.Phy.GigabitStatus)
        Write-Output ("  RMAC: snapshots={0} all={1} good_e={2} good_p={3} error={4} overflow={5} fcs={6} fragment={7}" -f
            $snapshot.Rmac.Snapshots, $snapshot.Rmac.RxAllFrames,
            $snapshot.Rmac.RxGoodEFrames, $snapshot.Rmac.RxGoodPFrames,
            $snapshot.Rmac.RxErrorFrames, $snapshot.Rmac.RxOverflow,
            $snapshot.Rmac.RxFcsErrorRaw, $snapshot.Rmac.RxFragmentErrorRaw)
        Write-Output ("  Driver RX: calls={0} ok={1} empty={2} fail={3} pbuf_fail={4} irq_complete={5} irq_lost={6}" -f
            $snapshot.Rmac.DriverRxCalls, $snapshot.Rmac.DriverRxOk,
            $snapshot.Rmac.DriverRxEmpty, $snapshot.Rmac.DriverRxFail,
            $snapshot.Rmac.DriverRxPbufAllocFail, $snapshot.Rmac.IrqRxComplete,
            $snapshot.Rmac.IrqRxMessageLost)
        Write-Output ("  lwIP tcpip input: memp_alloc_fail={0} mbox_fail={1}" -f
            $snapshot.Rmac.LwipTcpipInpktAllocFail, $snapshot.Rmac.LwipTcpipInpktMboxFail)
        Write-Output ("  iiod perf: state={0} schema={1} bytes={2}/{3} elapsed_ms={4} Mbps={5} reads={6} chunks={7} recv={8} fills={9} SO_RCVBUF={10}" -f
            $snapshot.IiodPerf.State, $snapshot.IiodPerf.SchemaVersion,
            $snapshot.IiodPerf.BytesReceived, $snapshot.IiodPerf.TargetBytes,
            $snapshot.IiodPerf.ElapsedMs,
            ([math]::Round([double]$snapshot.IiodPerf.PayloadMbpsX1000 / 1000.0, 3)),
            $snapshot.IiodPerf.ReadRequests, $snapshot.IiodPerf.ReadChunks,
            $snapshot.IiodPerf.RecvCalls, $snapshot.IiodPerf.CacheFills,
            $snapshot.IiodPerf.RcvbufSetsockoptResult)
        Write-Output ("  IQ: initialized={0} active={1} session={2} packets={3} bytes={4} gaps={5} reordered={6} invalid={7} rate={8} Mbps" -f
            $snapshot.Iq.Initialized, $snapshot.Iq.Active, $snapshot.Iq.SessionId, $snapshot.Iq.Packets,
            $snapshot.Iq.PayloadBytes, $snapshot.Iq.SequenceGaps,
            $snapshot.Iq.Reordered, $snapshot.Iq.Invalid,
            ([math]::Round([double]$snapshot.Iq.MbpsX1000 / 1000.0, 3)))
        Write-Output ("  IQ CRC32C: actual=0x{0:X8} expected=0x{1:X8} errors={2} flags=0x{3:X8}" -f
            $snapshot.Iq.Crc32c, $snapshot.Iq.ExpectedCrc32c,
            $snapshot.Iq.CrcErrors, $snapshot.Iq.CrcFlags)
        Write-Output ("  IQ CRC backend={0} self_test={1} updates={2} cycles_total={3} cycles_max={4}" -f
            $snapshot.Iq.CrcBackend, $snapshot.Iq.CrcHardwareSelfTest,
            $snapshot.Iq.CrcUpdates, $snapshot.Iq.CrcCyclesTotal,
            $snapshot.Iq.CrcCyclesMax)
        Write-Output ("  SDRC: state={0} request={1} session={2} center={3} complete={4} tx/rx={5}/{6} retries={7} timeouts={8} status={9}" -f
            $snapshot.SdrControl.State, $snapshot.SdrControl.RequestId,
            $snapshot.SdrControl.SessionId, $snapshot.SdrControl.CenterIndex,
            $snapshot.SdrControl.CompletedWindows, $snapshot.SdrControl.TxDatagrams,
            $snapshot.SdrControl.RxDatagrams, $snapshot.SdrControl.Retries,
            $snapshot.SdrControl.Timeouts, $snapshot.SdrControl.LastStatus)
        Write-Output ("  SDRC remote us: request_rx={0} tune={1}->{2} capture={3}->{4}; rate={5} Mbps crc=0x{6:X8}" -f
            $snapshot.SdrControl.AgentRequestRxUs,
            $snapshot.SdrControl.TuneStartUs,
            $snapshot.SdrControl.TuneCompleteUs,
            $snapshot.SdrControl.CaptureStartUs,
            $snapshot.SdrControl.CaptureCompleteUs,
            ([math]::Round([double]$snapshot.SdrControl.ActualPayloadMbpsX1000 / 1000.0, 3)),
            $snapshot.SdrControl.WindowCrc32c)
        Write-Output ("  SDRC prefetch: state={0} request/session/center={1}/{2}/{3} promoted={4}; recovered missing complete/ready={5}/{6}; iqsc_credit_proofs={7}" -f
            $snapshot.SdrControl.PrefetchState,
            $snapshot.SdrControl.PrefetchedRequestId,
            $snapshot.SdrControl.PrefetchedSessionId,
            $snapshot.SdrControl.PrefetchedCenterIndex,
            $snapshot.SdrControl.PrefetchedWindows,
            $snapshot.SdrControl.MissingCaptureComplete,
            $snapshot.SdrControl.PrefetchCreditWithoutReady,
            $snapshot.SdrControl.PrefetchIqscCreditProofs)
        Write-Output ("  Ring: queued={0}/{1} pushed={2} popped={3} full_drops={4} oversize_drops={5} high_watermark={6}" -f
            $snapshot.Ring.Queued, $snapshot.Ring.Capacity, $snapshot.Ring.Pushed,
            $snapshot.Ring.Popped, $snapshot.Ring.FullDrops,
            $snapshot.Ring.OversizeDrops, $snapshot.Ring.HighWatermark)
        $index++
    }
    if ($Report.Interval) {
        $interval = $Report.Interval
        Write-Output ("Interval: {0}s session_changed={1} rmac_refreshes={2} rmac_rx={3} iq_packets={4} ({5}/s) payload={6} Mbps" -f
            $interval.Seconds, $interval.SessionChanged, $interval.RmacSnapshotDelta,
            $interval.RmacRxAllFrameDelta, $interval.IqPacketDelta,
            $interval.IqPacketRate, $interval.IqPayloadMbps)
        Write-Output ("  IQ deltas: gaps={0} reordered={1} invalid={2} crc_errors={3}; ring drops: full={4} oversize={5}" -f
            $interval.IqSequenceGapDelta, $interval.IqReorderedDelta,
            $interval.IqInvalidDelta, $interval.IqCrcErrorDelta,
            $interval.RingFullDropDelta, $interval.RingOversizeDropDelta)
        Write-Output ("  lwIP input failures: memp={0} mbox={1}" -f
            $interval.LwipTcpipInpktAllocFailDelta,
            $interval.LwipTcpipInpktMboxFailDelta)
        Write-Output ("  SDRC deltas: complete={0} tx={1} rx={2} retries={3} timeouts={4}" -f
            $interval.SdrControlCompletedWindowDelta,
            $interval.SdrControlTxDelta, $interval.SdrControlRxDelta,
            $interval.SdrControlRetryDelta, $interval.SdrControlTimeoutDelta)
        Write-Output ("  SDRC recovery deltas: missing_complete={0} credit_without_ready={1} iqsc_credit_proofs={2}" -f
            $interval.SdrControlMissingCaptureCompleteDelta,
            $interval.SdrControlPrefetchCreditWithoutReadyDelta,
            $interval.SdrControlPrefetchIqscCreditProofDelta)
    }
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

$config = Read-HostConfig
$serial = Resolve-Probe $ProbeSerial $config
$jlink = Resolve-JLink $JLinkExe $config
$nm = Resolve-Nm $NmExe $config
$elfPath = Resolve-Cpu0Elf $Cpu0Elf
$elf = Get-ElfRecord $elfPath
$symbols = Get-ElfSymbols $nm $elfPath

$snapshots = New-Object System.Collections.Generic.List[object]
$first = Read-LiveSnapshot $jlink $serial $symbols
[void] $snapshots.Add($first.Snapshot)
$hostElapsed = $first.HostElapsedSeconds
if ($WindowSeconds -gt 0) {
    # Close Commander between snapshots so both cores run during the interval.
    Start-Sleep -Seconds $WindowSeconds
    $second = Read-LiveSnapshot $jlink $serial $symbols
    [void] $snapshots.Add($second.Snapshot)
    $hostElapsed += $WindowSeconds + $second.HostElapsedSeconds
}
$interval = if ($WindowSeconds -gt 0) {
    $firstTime = [DateTimeOffset]::Parse($snapshots[0].TimestampUtc)
    $secondTime = [DateTimeOffset]::Parse($snapshots[1].TimestampUtc)
    Get-Interval $snapshots[0] $snapshots[1] ($secondTime - $firstTime).TotalSeconds
}
else { $null }

$symbolReport = [ordered]@{}
foreach ($key in $symbols.Keys) {
    $symbolReport[$key] = [ordered]@{
        Name = $symbols[$key].Name
        Address = Format-Hex32 $symbols[$key].Address
        Size = $symbols[$key].Size
        Type = $symbols[$key].Type
    }
}
$report = [ordered]@{
    Tool = 'ra8p1-cpu0-net-stats'
    ToolVersion = '1.3'
    TimestampUtc = (Get-Date).ToUniversalTime().ToString('o')
    ProbeSerial = $serial
    Target = 'R7KA8P1KF_CPU0'
    JLinkExe = $jlink
    NmExe = $nm
    Elf = $elf
    Symbols = $symbolReport
    SnapshotCount = $snapshots.Count
    RequestedWindowSeconds = $WindowSeconds
    HostElapsedSeconds = $hostElapsed
    EvidenceBoundary = 'ELF SHA-256 binds symbol interpretation; it is not proof of flashed image identity. Halt-based SWD can perturb live Ethernet traffic.'
    Snapshots = @($snapshots | ForEach-Object { $_ })
    Interval = $interval
}

if ($Json) {
    $report | ConvertTo-Json -Depth 9
}
else {
    Write-HumanReport $report
}

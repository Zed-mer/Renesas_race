<#
.SYNOPSIS
Reads the 128-entry CPU0 per-window trace ring over read-only SWD.

.DESCRIPTION
The exact CPU0 ELF supplies the address and size of g_cpu0_trace_ring. J-Link
briefly halts CPU0, saves the non-cached SDRAM object, resumes CPU0, and exits.
Run this between timed traffic intervals because any debugger halt perturbs a
live Ethernet stream.
#>
[CmdletBinding()]
param(
    [string] $ProbeSerial,
    [string] $JLinkExe,
    [string] $NmExe,
    [string] $Cpu0Elf,
    [switch] $Json,
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ProjectRoot = Split-Path -Parent $PSScriptRoot
$layoutHelper = Join-Path $PSScriptRoot 'project-layout.ps1'
. $layoutHelper
$script:Cpu0Project = (Resolve-Ra8p1ProjectLayout -Solution $script:ProjectRoot).Cpu0Directory
$script:TraceMagic = [uint32]0x30435254
$script:TraceVersion = 3
$script:TraceCapacity = 128
$script:TraceControlBytes = 32
$script:TraceRecordBytes = 208
$script:TraceBytes = $script:TraceControlBytes +
    ($script:TraceCapacity * $script:TraceRecordBytes)

function Read-HostConfig {
    $path = Join-Path $HOME '.codex\ra8p1.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return @{} }
    $object = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    $result = @{}
    foreach ($property in $object.PSObject.Properties) {
        $result[$property.Name] = [string]$property.Value
    }
    return $result
}

function Resolve-Probe([string]$Requested, [hashtable]$Config) {
    $candidate = if ($Requested) { $Requested }
        elseif ($env:RA8P1_PROBE_SERIAL) { $env:RA8P1_PROBE_SERIAL }
        else { [string]$Config['ProbeSerial'] }
    if (-not $candidate -or $candidate -notmatch '^[0-9]{6,20}$') {
        throw 'Pass a 6-20 digit -ProbeSerial or set RA8P1_PROBE_SERIAL.'
    }
    return $candidate.TrimStart('0')
}

function Resolve-Executable([string[]]$Candidates, [string]$Description) {
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "$Description was not found. Pass its explicit path."
}

function Resolve-JLink([string]$Requested, [hashtable]$Config) {
    $command = Get-Command JLink.exe -ErrorAction SilentlyContinue
    return Resolve-Executable @(
        $Requested,
        $(if ($env:RA8P1_JLINK_ROOT) { Join-Path $env:RA8P1_JLINK_ROOT 'JLink.exe' }),
        $(if ($Config['JLinkRoot']) { Join-Path $Config['JLinkRoot'] 'JLink.exe' }),
        $(if ($command) { $command.Source }),
        'C:\Program Files\SEGGER\JLink_V956\JLink.exe',
        'C:\Program Files\SEGGER\JLink\JLink.exe'
    ) 'JLink.exe'
}

function Resolve-Nm([string]$Requested, [hashtable]$Config) {
    $command = Get-Command arm-none-eabi-nm.exe -ErrorAction SilentlyContinue
    return Resolve-Executable @(
        $Requested,
        $(if ($env:RA8P1_E2_ROOT) {
            Join-Path $env:RA8P1_E2_ROOT 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe'
        }),
        $(if ($Config['E2Root']) {
            Join-Path $Config['E2Root'] 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe'
        }),
        $(if ($command) { $command.Source }),
        'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe'
    ) 'arm-none-eabi-nm.exe'
}

function Resolve-Elf([string]$Requested) {
    $path = if ($Requested) { [IO.Path]::GetFullPath($Requested) }
        else { Join-Path $script:Cpu0Project 'Debug\rtthread.elf' }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "CPU0 ELF does not exist: $path"
    }
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-TraceSymbol([string]$Nm, [string]$Elf) {
    $lines = @(& $Nm -S -n $Elf 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "arm-none-eabi-nm failed.`n$($lines -join "`n")"
    }
    foreach ($line in $lines) {
        $match = [regex]::Match([string]$line,
            '^\s*(?<address>[0-9A-Fa-f]+)\s+(?<size>[0-9A-Fa-f]+)\s+\S\s+g_cpu0_trace_ring\s*$')
        if ($match.Success) {
            $size = [uint32][Convert]::ToUInt64($match.Groups['size'].Value, 16)
            if ($size -ne $script:TraceBytes) {
                throw "g_cpu0_trace_ring is $size bytes; expected $($script:TraceBytes)."
            }
            return [pscustomobject]@{
                Address = [uint32][Convert]::ToUInt64($match.Groups['address'].Value, 16)
                Size = $size
            }
        }
    }
    throw 'g_cpu0_trace_ring was not found in the exact CPU0 ELF.'
}

function Get-U16([byte[]]$Bytes, [int]$Offset) {
    return [BitConverter]::ToUInt16($Bytes, $Offset)
}

function Get-U32([byte[]]$Bytes, [int]$Offset) {
    return [BitConverter]::ToUInt32($Bytes, $Offset)
}

function Get-U64([byte[]]$Bytes, [int]$Offset) {
    return [BitConverter]::ToUInt64($Bytes, $Offset)
}

function Get-CycleDeltaMs([uint32]$Start, [uint32]$End) {
    $delta = if ($End -ge $Start) { [uint64]$End - $Start }
        else { [uint64]4294967296 + $End - $Start }
    return [math]::Round([double]$delta / 1000000.0, 6)
}

function Get-CycleCountMs([uint32]$Cycles) {
    return [math]::Round([double]$Cycles / 1000000.0, 6)
}

function Get-RemoteDeltaMs([uint64]$Start, [uint64]$End) {
    if ($Start -eq 0 -or $End -lt $Start) { return $null }
    return [math]::Round([double]($End - $Start) / 1000.0, 6)
}

function Decode-Trace([byte[]]$Bytes) {
    if ($Bytes.Length -ne $script:TraceBytes) {
        throw "Trace dump is $($Bytes.Length) bytes; expected $($script:TraceBytes)."
    }
    $magic = Get-U32 $Bytes 0
    $version = Get-U16 $Bytes 4
    $recordBytes = Get-U16 $Bytes 6
    $capacity = Get-U32 $Bytes 8
    if ($magic -ne $script:TraceMagic -or $version -ne $script:TraceVersion -or
        $recordBytes -ne $script:TraceRecordBytes -or
        $capacity -ne $script:TraceCapacity) {
        throw ('Trace ABI mismatch: magic=0x{0:X8} version={1} record={2} capacity={3}.' -f
            $magic, $version, $recordBytes, $capacity)
    }
    $records = New-Object System.Collections.Generic.List[object]
    for ($index = 0; $index -lt $script:TraceCapacity; $index++) {
        $base = $script:TraceControlBytes + ($index * $script:TraceRecordBytes)
        $begin = Get-U32 $Bytes $base
        $end = Get-U32 $Bytes ($base + 200)
        $session = Get-U32 $Bytes ($base + 8)
        if ($begin -eq 0 -or ($begin -band 1) -ne 0 -or
            $begin -ne $end -or $session -eq 0) { continue }
        $flags = Get-U32 $Bytes ($base + 24)
        $requestCycles = Get-U32 $Bytes ($base + 32)
        $firstCycles = Get-U32 $Bytes ($base + 36)
        $lastCycles = Get-U32 $Bytes ($base + 40)
        $crcCyclesAt = Get-U32 $Bytes ($base + 44)
        $ackCycles = Get-U32 $Bytes ($base + 48)
        $stftStart = Get-U32 $Bytes ($base + 52)
        $stftEnd = Get-U32 $Bytes ($base + 56)
        $npuStart = Get-U32 $Bytes ($base + 60)
        $npuEnd = Get-U32 $Bytes ($base + 64)
        $visible = Get-U32 $Bytes ($base + 68)
        $agentRx = Get-U64 $Bytes ($base + 72)
        $tuneStart = Get-U64 $Bytes ($base + 80)
        $tuneEnd = Get-U64 $Bytes ($base + 88)
        $captureStart = Get-U64 $Bytes ($base + 96)
        $captureEnd = Get-U64 $Bytes ($base + 104)
        $captureReadyCycles = Get-U32 $Bytes ($base + 156)
        $captureCompleteCycles = Get-U32 $Bytes ($base + 160)
        $creditAcceptedCycles = Get-U32 $Bytes ($base + 164)
        $iqscStartCycles = Get-U32 $Bytes ($base + 168)
        $v2InputCopyCycles = Get-U32 $Bytes ($base + 172)
        $v2InvokeCycles = Get-U32 $Bytes ($base + 176)
        $v2OutputCopyCycles = Get-U32 $Bytes ($base + 180)
        $v3InputCopyCycles = Get-U32 $Bytes ($base + 184)
        $v3InvokeCycles = Get-U32 $Bytes ($base + 188)
        $v3OutputCopyCycles = Get-U32 $Bytes ($base + 192)
        $postprocessCycles = Get-U32 $Bytes ($base + 196)
        $npuPhasesValid = ($flags -band 0x10000) -ne 0
        $postprocessValid = ($flags -band 0x20000) -ne 0
        $modelPhaseCycles = [uint64]$v2InputCopyCycles + $v2InvokeCycles +
            $v2OutputCopyCycles + $v3InputCopyCycles + $v3InvokeCycles +
            $v3OutputCopyCycles
        [void]$records.Add([ordered]@{
            Slot = $index
            Sequence = $begin
            RequestId = Get-U32 $Bytes ($base + 4)
            SessionId = $session
            CenterIndex = Get-U32 $Bytes ($base + 12)
            WindowIndex = Get-U32 $Bytes ($base + 16)
            SampleCount = Get-U32 $Bytes ($base + 20)
            Flags = ('0x{0:X8}' -f $flags)
            Status = Get-U32 $Bytes ($base + 28)
            RequestTxCycles = $requestCycles
            FirstPacketCycles = $firstCycles
            LastPacketCycles = $lastCycles
            CrcCompleteCycles = $crcCyclesAt
            AckTxCycles = $ackCycles
            StftStartCycles = $stftStart
            StftEndCycles = $stftEnd
            NpuStartCycles = $npuStart
            NpuEndCycles = $npuEnd
            Cpu1VisibleCycles = $visible
            CaptureReadyCycles = $captureReadyCycles
            CaptureCompleteCycles = $captureCompleteCycles
            CreditAcceptedCycles = $creditAcceptedCycles
            IqscStartCycles = $iqscStartCycles
            V2InputCopyCycles = $v2InputCopyCycles
            V2InvokeCycles = $v2InvokeCycles
            V2OutputCopyCycles = $v2OutputCopyCycles
            V3InputCopyCycles = $v3InputCopyCycles
            V3InvokeCycles = $v3InvokeCycles
            V3OutputCopyCycles = $v3OutputCopyCycles
            PostprocessCycles = $postprocessCycles
            V2InputCopyMs = if ($npuPhasesValid) {
                Get-CycleCountMs $v2InputCopyCycles
            } else { $null }
            V2InvokeMs = if ($npuPhasesValid) {
                Get-CycleCountMs $v2InvokeCycles
            } else { $null }
            V2OutputCopyMs = if ($npuPhasesValid) {
                Get-CycleCountMs $v2OutputCopyCycles
            } else { $null }
            V3InputCopyMs = if ($npuPhasesValid) {
                Get-CycleCountMs $v3InputCopyCycles
            } else { $null }
            V3InvokeMs = if ($npuPhasesValid) {
                Get-CycleCountMs $v3InvokeCycles
            } else { $null }
            V3OutputCopyMs = if ($npuPhasesValid) {
                Get-CycleCountMs $v3OutputCopyCycles
            } else { $null }
            ModelPhaseSumMs = if ($npuPhasesValid) {
                Get-CycleCountMs ([uint32]$modelPhaseCycles)
            } else { $null }
            PostprocessMs = if ($postprocessValid) {
                Get-CycleCountMs $postprocessCycles
            } else { $null }
            RequestToCaptureReadyMs = if (($flags -band 0x1001) -eq 0x1001) {
                Get-CycleDeltaMs $requestCycles $captureReadyCycles
            } else { $null }
            CaptureReadyToIqscStartMs = if (($flags -band 0x9000) -eq 0x9000) {
                Get-CycleDeltaMs $captureReadyCycles $iqscStartCycles
            } else { $null }
            IqscStartToCaptureCompleteMs = if (($flags -band 0xA000) -eq 0xA000) {
                Get-CycleDeltaMs $iqscStartCycles $captureCompleteCycles
            } else { $null }
            CaptureCompleteToCreditAcceptedMs = if (($flags -band 0x6000) -eq 0x6000) {
                Get-CycleDeltaMs $captureCompleteCycles $creditAcceptedCycles
            } else { $null }
            AckToCreditAcceptedMs = if (($flags -band 0x4020) -eq 0x4020) {
                Get-CycleDeltaMs $ackCycles $creditAcceptedCycles
            } else { $null }
            RequestToIqscStartMs = if (($flags -band 0x8001) -eq 0x8001) {
                Get-CycleDeltaMs $requestCycles $iqscStartCycles
            } else { $null }
            RequestToFirstPacketMs = if (($flags -band 0x5) -eq 0x5) {
                Get-CycleDeltaMs $requestCycles $firstCycles
            } else { $null }
            FirstToLastPacketMs = if (($flags -band 0xC) -eq 0xC) {
                Get-CycleDeltaMs $firstCycles $lastCycles
            } else { $null }
            LastPacketToCrcCompleteMs = if (($flags -band 0x18) -eq 0x18) {
                Get-CycleDeltaMs $lastCycles $crcCyclesAt
            } else { $null }
            RequestToCrcCompleteMs = if (($flags -band 0x11) -eq 0x11) {
                Get-CycleDeltaMs $requestCycles $crcCyclesAt
            } else { $null }
            StftMs = if (($flags -band 0xC0) -eq 0xC0) {
                Get-CycleDeltaMs $stftStart $stftEnd
            } else { $null }
            NpuMs = if (($flags -band 0x300) -eq 0x300) {
                Get-CycleDeltaMs $npuStart $npuEnd
            } else { $null }
            FirstPacketToNpuResultMs = if (($flags -band 0x204) -eq 0x204) {
                Get-CycleDeltaMs $firstCycles $npuEnd
            } else { $null }
            RequestToNpuResultMs = if (($flags -band 0x201) -eq 0x201) {
                Get-CycleDeltaMs $requestCycles $npuEnd
            } else { $null }
            NpuToCpu1VisibleUpperMs = if (($flags -band 0x600) -eq 0x600) {
                Get-CycleDeltaMs $npuEnd $visible
            } else { $null }
            RequestToCpu1VisibleUpperMs = if (($flags -band 0x401) -eq 0x401) {
                Get-CycleDeltaMs $requestCycles $visible
            } else { $null }
            RequestToAckMs = if (($flags -band 0x21) -eq 0x21) {
                Get-CycleDeltaMs $requestCycles $ackCycles
            } else { $null }
            NpuToAckMs = if (($flags -band 0x220) -eq 0x220) {
                Get-CycleDeltaMs $npuEnd $ackCycles
            } else { $null }
            RemoteRequestToTuneStartMs = Get-RemoteDeltaMs $agentRx $tuneStart
            RemoteRequestToCaptureStartMs = Get-RemoteDeltaMs $agentRx $captureStart
            RemoteTuneMs = Get-RemoteDeltaMs $tuneStart $tuneEnd
            RemoteCaptureMs = Get-RemoteDeltaMs $captureStart $captureEnd
            # Subtracting the SDR-side queue/tune duration from CPU0
            # request->result leaves a conservative capture-start bound.  It
            # still includes the unknown one-way control latency from CPU0 to
            # the SDR and is therefore never labelled as an exact duration.
            CaptureStartToNpuUpperMs = if ((($flags -band 0x203) -eq 0x203) -and
                $captureStart -ge $agentRx -and $agentRx -ne 0) {
                $requestToNpu = Get-CycleDeltaMs $requestCycles $npuEnd
                $beforeCapture = Get-RemoteDeltaMs $agentRx $captureStart
                if ($null -ne $beforeCapture -and $requestToNpu -ge $beforeCapture) {
                    [math]::Round($requestToNpu - $beforeCapture, 6)
                } else { $null }
            } else { $null }
            AgentRequestRxUs = $agentRx
            TuneStartUs = $tuneStart
            TuneCompleteUs = $tuneEnd
            CaptureStartUs = $captureStart
            CaptureCompleteUs = $captureEnd
            PayloadMbpsX1000 = Get-U32 $Bytes ($base + 112)
            WindowCrc32c = ('0x{0:X8}' -f (Get-U32 $Bytes ($base + 116)))
            CrcCycles = Get-U32 $Bytes ($base + 120)
            SequenceGaps = Get-U32 $Bytes ($base + 124)
            Reordered = Get-U32 $Bytes ($base + 128)
            InvalidPackets = Get-U32 $Bytes ($base + 132)
            RingFullDrops = Get-U32 $Bytes ($base + 136)
            RingOversizeDrops = Get-U32 $Bytes ($base + 140)
            RingHighWatermark = Get-U32 $Bytes ($base + 144)
            RingFree = Get-U32 $Bytes ($base + 148)
            Cpu0LoadPermille = Get-U32 $Bytes ($base + 152)
        })
    }
    return [ordered]@{
        Magic = ('0x{0:X8}' -f $magic)
        Version = $version
        RecordBytes = $recordBytes
        Capacity = $capacity
        CpuCycleHz = Get-U32 $Bytes 12
        RecordsStarted = Get-U32 $Bytes 16
        RecordsOverwritten = Get-U32 $Bytes 20
        LatestSequence = Get-U32 $Bytes 24
        BootCount = Get-U32 $Bytes 28
        ValidRecords = $records.Count
        Records = $records
    }
}

function Invoke-TraceRead([string]$JLink, [string]$Serial, $Symbol) {
    $dump = Join-Path $env:TEMP ("ra8p1-trace-{0}.bin" -f [guid]::NewGuid().ToString('N'))
    $commands = @(
        'speed 4000', 'speed 4000', 'halt',
        # J-Link Commander parses bare numbers as hexadecimal. Make the byte
        # count explicit so the decimal trace size is not reinterpreted.
        ('savebin {0}, 0x{1:X8}, 0x{2:X}' -f $dump, $Symbol.Address, $Symbol.Size),
        'go', 'exit'
    )
    $arguments = @('-NoGui','1','-SelectEmuBySN',$Serial,'-Device','R7KA8P1KF_CPU0',
                   '-If','SWD','-Speed','4000','-AutoConnect','1')
    $lines = New-Object System.Collections.Generic.List[string]
    try {
        $saved = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        ((($commands -join "`r`n") + "`r`n" | & $JLink @arguments 2>&1) |
            ForEach-Object { [void]$lines.Add([string]$_) })
        $exitCode = $LASTEXITCODE
        $ErrorActionPreference = $saved
        $text = $lines -join "`n"
        if ($exitCode -ne 0 -or $text -match 'Cannot connect|Failed to connect|Could not find emulator|No emulator connected') {
            throw "J-Link trace read failed with exit code $exitCode.`n$text"
        }
        if ($text -notmatch ('(?m)^\s*(?:J-Link>\s*)?S/N:\s*0*' +
            [regex]::Escape($Serial) + '\s*$') -or
            $text -notmatch 'Cortex-M85 identified') {
            throw 'J-Link output did not prove the selected probe and CPU0 target.'
        }
        if (-not (Test-Path -LiteralPath $dump -PathType Leaf)) {
            throw 'J-Link did not create the trace dump.'
        }
        return [IO.File]::ReadAllBytes($dump)
    }
    finally {
        if (Test-Path -LiteralPath $dump) { Remove-Item -LiteralPath $dump -Force }
    }
}

function Invoke-SelfTest {
    $bytes = New-Object byte[] $script:TraceBytes
    [Array]::Copy([BitConverter]::GetBytes($script:TraceMagic), 0, $bytes, 0, 4)
    [Array]::Copy([BitConverter]::GetBytes([uint16]$script:TraceVersion), 0, $bytes, 4, 2)
    [Array]::Copy([BitConverter]::GetBytes([uint16]$script:TraceRecordBytes), 0, $bytes, 6, 2)
    [Array]::Copy([BitConverter]::GetBytes([uint32]$script:TraceCapacity), 0, $bytes, 8, 4)
    [Array]::Copy([BitConverter]::GetBytes([uint32]1000000000), 0, $bytes, 12, 4)
    $base = $script:TraceControlBytes
    foreach ($pair in @(
        @(0, 2), @(4, 101), @(8, 201), @(12, 2), @(16, 0), @(20, 590336),
        @(24, 0x0003F7FF), @(32, 1000), @(36, 2000), @(40, 3000), @(44, 3100),
        @(48, 6500),
        @(52, 4000), @(56, 5000), @(60, 5100), @(64, 9000), @(68, 10000),
        @(112, 390000), @(120, 1234), @(144, 1598), @(148, 2498),
        @(152, 990), @(156, 1500), @(160, 3500), @(164, 6800),
        @(168, 1800), @(172, 100), @(176, 1000), @(180, 200),
        @(184, 100), @(188, 1000), @(192, 100), @(196, 500),
        @(200, 2))) {
        [Array]::Copy([BitConverter]::GetBytes([uint32]$pair[1]), 0,
                      $bytes, $base + [int]$pair[0], 4)
    }
    $decoded = Decode-Trace $bytes
    if ($decoded.ValidRecords -ne 1 -or
        $decoded.Records[0].SessionId -ne 201 -or
        $decoded.Records[0].SampleCount -ne 590336 -or
        $decoded.Records[0].RequestTxCycles -ne 1000 -or
        $decoded.Records[0].NpuEndCycles -ne 9000 -or
        $decoded.Records[0].Cpu1VisibleCycles -ne 10000 -or
        $decoded.Records[0].CaptureReadyCycles -ne 1500 -or
        $decoded.Records[0].IqscStartCycles -ne 1800 -or
        $decoded.Records[0].V2InvokeCycles -ne 1000 -or
        $decoded.Records[0].V3InvokeCycles -ne 1000 -or
        $decoded.Records[0].PostprocessMs -ne 0.0005 -or
        $decoded.Records[0].AckToCreditAcceptedMs -ne 0.0003) {
        throw 'Trace self-test failed.'
    }
    Write-Output 'Self-test passed: trace v3 ABI, inference phases and handoff-stage decoding.'
}

if ($SelfTest) { Invoke-SelfTest; return }

$config = Read-HostConfig
$serial = Resolve-Probe $ProbeSerial $config
$jlink = Resolve-JLink $JLinkExe $config
$nm = Resolve-Nm $NmExe $config
$elf = Resolve-Elf $Cpu0Elf
$symbol = Get-TraceSymbol $nm $elf
$trace = Decode-Trace (Invoke-TraceRead $jlink $serial $symbol)
$report = [ordered]@{
    EvidenceKind = 'measured/read-only SWD snapshot'
    Warning = 'Debugger halt perturbs live traffic; capture only between timed intervals.'
    ProbeSerial = $serial
    Target = 'R7KA8P1KF_CPU0'
    Elf = [ordered]@{
        Path = $elf
        Sha256 = (Get-FileHash -LiteralPath $elf -Algorithm SHA256).Hash.ToUpperInvariant()
        Size = (Get-Item -LiteralPath $elf).Length
        TimestampUtc = (Get-Item -LiteralPath $elf).LastWriteTimeUtc.ToString('o')
    }
    Symbol = [ordered]@{
        Name = 'g_cpu0_trace_ring'
        Address = ('0x{0:X8}' -f $symbol.Address)
        Size = $symbol.Size
    }
    Trace = $trace
}

if ($Json) {
    $report | ConvertTo-Json -Depth 8
}
else {
    Write-Output 'RA8P1 CPU0 128-entry per-window trace (read-only SWD)'
    Write-Output ("Probe={0} ELF_SHA256={1}" -f $serial, $report.Elf.Sha256)
    Write-Output ("records={0}/{1} overwritten={2} boot={3}" -f
        $trace.ValidRecords, $trace.Capacity, $trace.RecordsOverwritten,
        $trace.BootCount)
    foreach ($record in $trace.Records) {
        Write-Output ("session={0} request={1} center={2} window={3} rate={4} Mbps STFT={5} ms NPU={6} ms V2={7}/{8}/{9} ms V3={10}/{11}/{12} ms post={13} ms first->result={14} ms ready->start={15} ms ack->credit={16} ms gaps/drop/crc={17}/{18}/{19}" -f
            $record.SessionId, $record.RequestId, $record.CenterIndex,
            $record.WindowIndex,
            ([math]::Round($record.PayloadMbpsX1000 / 1000.0, 3)),
            $record.StftMs, $record.NpuMs,
            $record.V2InputCopyMs, $record.V2InvokeMs,
            $record.V2OutputCopyMs, $record.V3InputCopyMs,
            $record.V3InvokeMs, $record.V3OutputCopyMs,
            $record.PostprocessMs, $record.FirstPacketToNpuResultMs,
            $record.CaptureReadyToIqscStartMs,
            $record.AckToCreditAcceptedMs,
            $record.SequenceGaps,
            ($record.RingFullDrops + $record.RingOversizeDrops),
            $record.WindowCrc32c)
    }
}

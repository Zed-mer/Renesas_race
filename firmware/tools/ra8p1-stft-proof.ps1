<#
.SYNOPSIS
Reads the one-shot CPU0 synthetic STFT proof over read-only SWD.

.DESCRIPTION
The symbol addresses and sizes come from the exact CPU0 ELF. The proof covers
the production S16-to-Q15/STFT compute path for one 590,336-complex-sample
window and reads the independent rfpipe stack-watermark ABI in the same halt.
Input generation, Ethernet receive, CRC, NPU inference and CPU0/CPU1 IPC are
explicitly excluded. J-Link briefly halts CPU0, reads the persistent object,
resumes CPU0 and exits; this script never loads or flashes firmware.
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
$script:SymbolName = 'g_analysis_stft_proof'
$script:ProofBytes = 320
$script:ProofWords = 80
$script:StackSymbolName = 'g_rf_pipeline_stack_proof'
$script:StackProofBytes = 32
$script:StackProofWords = 8
$script:StackProofMagic = [uint32] 0x52535046
$script:StackProofDoneMagic = [uint32] 0x5253444E
$script:StackProofVersion = 1
$script:ProofMagic = [uint32] 0x53544650
$script:ProofDoneMagic = [uint32] 0x5354444E
$script:ProofVersion = 1
$script:ProofPass = 2
$script:ProofRuns = 5
$script:RequiredFlags = [uint32] 0x000003FF
$script:Series = [ordered]@{
    FullWindow = 96
    StftHot = 128
    WindowApply = 160
    Fft = 192
    PowerReduce = 224
    PoolAndQuantize = 256
    IngestAndSchedule = 288
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
    $object = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    $config = @{}
    foreach ($property in $object.PSObject.Properties) {
        $config[$property.Name] = [string] $property.Value
    }
    return $config
}

function Resolve-Probe {
    param([string] $Requested, [hashtable] $Config)
    $candidate = if ($Requested) { $Requested }
        elseif ($env:RA8P1_PROBE_SERIAL) { $env:RA8P1_PROBE_SERIAL }
        else { [string] $Config['ProbeSerial'] }
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

function Get-ExactSymbol {
    param(
        [Parameter(Mandatory)] [string] $Nm,
        [Parameter(Mandatory)] [string] $Elf,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [uint32] $ExpectedBytes
    )
    $lines = @(& $Nm -S -n $Elf 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "arm-none-eabi-nm failed with exit code $LASTEXITCODE."
    }
    foreach ($line in $lines) {
        $match = [regex]::Match([string] $line,
            '^\s*(?<address>[0-9A-Fa-f]+)\s+(?<size>[0-9A-Fa-f]+)\s+(?<type>\S)\s+(?<name>\S+)\s*$')
        if ($match.Success -and ($match.Groups['name'].Value -eq $Name)) {
            $size = [uint32] ([Convert]::ToUInt64($match.Groups['size'].Value, 16))
            if ($size -ne $ExpectedBytes) {
                throw "$Name is $size bytes; expected exact ABI size $ExpectedBytes."
            }
            return [pscustomobject]@{
                Name = $Name
                Address = [uint32] ([Convert]::ToUInt64($match.Groups['address'].Value, 16))
                Size = $size
                Type = $match.Groups['type'].Value
            }
        }
    }
    throw "Required ELF symbol was not found: $Name"
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

function Convert-CyclesToMs {
    param([uint32] $Cycles, [uint32] $ClockHz)
    if ($ClockHz -eq 0) { return $null }
    return [math]::Round(([double] $Cycles * 1000.0) / [double] $ClockHz, 6)
}

function Get-SeriesRecord {
    param(
        [hashtable] $Map,
        [uint32] $Base,
        [int] $Offset,
        [uint32] $ClockHz
    )
    $samples = @()
    for ($i = 0; $i -lt $script:ProofRuns; $i++) {
        $samples += Get-U32 $Map $Base ($Offset + (4 * $i))
    }
    return [pscustomobject]@{
        SamplesCycles = @($samples)
        SamplesMs = @($samples | ForEach-Object { Convert-CyclesToMs $_ $ClockHz })
        MinimumCycles = Get-U32 $Map $Base ($Offset + 20)
        MedianCycles = Get-U32 $Map $Base ($Offset + 24)
        MaximumCycles = Get-U32 $Map $Base ($Offset + 28)
        MinimumMs = Convert-CyclesToMs (Get-U32 $Map $Base ($Offset + 20)) $ClockHz
        MedianMs = Convert-CyclesToMs (Get-U32 $Map $Base ($Offset + 24)) $ClockHz
        MaximumMs = Convert-CyclesToMs (Get-U32 $Map $Base ($Offset + 28)) $ClockHz
    }
}

function Get-ProofRecord {
    param([hashtable] $Map, [uint32] $Base)
    $clock = Get-U32 $Map $Base 20
    $series = [ordered]@{}
    foreach ($entry in $script:Series.GetEnumerator()) {
        $series[$entry.Key] = Get-SeriesRecord $Map $Base $entry.Value $clock
    }
    return [pscustomobject]@{
        Magic = Get-U32 $Map $Base 0
        Version = Get-U32 $Map $Base 4
        Status = Get-U32 $Map $Base 8
        CompletionMagic = Get-U32 $Map $Base 12
        Flags = Get-U32 $Map $Base 16
        CoreClockHz = $clock
        WarmupRuns = Get-U32 $Map $Base 24
        MeasuredRuns = Get-U32 $Map $Base 28
        ComplexSamples = Get-U32 $Map $Base 32
        InputFormat = Get-U32 $Map $Base 36
        ValidBits = Get-U32 $Map $Base 40
        FftSize = Get-U32 $Map $Base 44
        HopSize = Get-U32 $Map $Base 48
        StftFrames = Get-U32 $Map $Base 52
        FrequencyPool = Get-U32 $Map $Base 56
        TimePool = Get-U32 $Map $Base 60
        InputBlockSamples = Get-U32 $Map $Base 64
        Checksum = Get-U32 $Map $Base 68
        PeakBin = Get-U32 $Map $Base 72
        PeakPower = Get-U32 $Map $Base 76
        Cfsr = Get-U32 $Map $Base 80
        Hfsr = Get-U32 $Map $Base 84
        ChecksumMismatches = Get-U32 $Map $Base 88
        FrameMismatches = Get-U32 $Map $Base 92
        Series = [pscustomobject] $series
    }
}

function Assert-ProofRecord {
    param([Parameter(Mandatory)] $Proof)
    if ($Proof.Magic -ne $script:ProofMagic) {
        throw "STFT proof magic mismatch: $(Format-Hex32 $Proof.Magic)."
    }
    if ($Proof.Version -ne $script:ProofVersion) {
        throw "STFT proof version is $($Proof.Version); expected $($script:ProofVersion)."
    }
    if ($Proof.CompletionMagic -ne $script:ProofDoneMagic) {
        throw "STFT proof is incomplete: completion=$(Format-Hex32 $Proof.CompletionMagic)."
    }
    if ($Proof.Status -ne $script:ProofPass) {
        throw "STFT proof status is $(Format-Hex32 $Proof.Status), not PASS."
    }
    if (($Proof.Flags -band $script:RequiredFlags) -ne $script:RequiredFlags) {
        throw "STFT proof exclusion/path flags are incomplete: $(Format-Hex32 $Proof.Flags)."
    }
    if (($Proof.CoreClockHz -eq 0) -or ($Proof.WarmupRuns -ne 1) -or
        ($Proof.MeasuredRuns -ne $script:ProofRuns)) {
        throw 'STFT proof clock or warm-up/measured run definition is invalid.'
    }
    if (($Proof.ComplexSamples -ne 590336) -or ($Proof.ValidBits -ne 12) -or
        ($Proof.FftSize -ne 1024) -or ($Proof.HopSize -ne 512) -or
        ($Proof.StftFrames -ne 1152) -or ($Proof.FrequencyPool -ne 0) -or
        ($Proof.TimePool -ne 10) -or ($Proof.InputBlockSamples -ne 256)) {
        throw ("STFT proof contract mismatch: samples={0}, valid_bits={1}, fft/hop/frames={2}/{3}/{4}, frequency_pool={5}, time_pool={6}, input_block={7}." -f
               $Proof.ComplexSamples, $Proof.ValidBits, $Proof.FftSize,
               $Proof.HopSize, $Proof.StftFrames, $Proof.FrequencyPool,
               $Proof.TimePool, $Proof.InputBlockSamples)
    }
    if (($Proof.Checksum -eq 0) -or ($Proof.ChecksumMismatches -ne 0) -or
        ($Proof.FrameMismatches -ne 0)) {
        throw 'STFT proof checksum/frame consistency failed.'
    }
    if (($Proof.Cfsr -ne 0) -or ($Proof.Hfsr -ne 0)) {
        throw "STFT proof fault registers are nonzero: CFSR=$(Format-Hex32 $Proof.Cfsr), HFSR=$(Format-Hex32 $Proof.Hfsr)."
    }

    foreach ($entry in $script:Series.GetEnumerator()) {
        $record = $Proof.Series.($entry.Key)
        if (($record.SamplesCycles.Count -ne $script:ProofRuns) -or
            ($record.SamplesCycles | Where-Object { $_ -eq 0 })) {
            throw "$($entry.Key) does not contain five positive raw cycle samples."
        }
        $ordered = @($record.SamplesCycles | Sort-Object { [uint64] $_ })
        if (($record.MinimumCycles -ne $ordered[0]) -or
            ($record.MedianCycles -ne $ordered[2]) -or
            ($record.MaximumCycles -ne $ordered[4])) {
            throw "$($entry.Key) stored min/median/max does not match its raw samples."
        }
    }
    for ($i = 0; $i -lt $script:ProofRuns; $i++) {
        $stageSum = [uint64] $Proof.Series.WindowApply.SamplesCycles[$i] +
            [uint64] $Proof.Series.Fft.SamplesCycles[$i] +
            [uint64] $Proof.Series.PowerReduce.SamplesCycles[$i] +
            [uint64] $Proof.Series.PoolAndQuantize.SamplesCycles[$i]
        if ($stageSum -ne [uint64] $Proof.Series.StftHot.SamplesCycles[$i]) {
            throw "Run $i STFT stage sum does not equal the hot-path total."
        }
        $fullSum = [uint64] $Proof.Series.StftHot.SamplesCycles[$i] +
            [uint64] $Proof.Series.IngestAndSchedule.SamplesCycles[$i]
        if ($fullSum -ne [uint64] $Proof.Series.FullWindow.SamplesCycles[$i]) {
            throw "Run $i ingest + hot-path sum does not equal the full-window total."
        }
    }
}

function Get-StackProofRecord {
    param([hashtable] $Map, [uint32] $Base)
    return [pscustomobject]@{
        Magic = Get-U32 $Map $Base 0
        Version = Get-U32 $Map $Base 4
        CompletionMagic = Get-U32 $Map $Base 8
        StackBytes = Get-U32 $Map $Base 12
        UsedHighWaterBytes = Get-U32 $Map $Base 16
        FreeLowWaterBytes = Get-U32 $Map $Base 20
        Observations = Get-U32 $Map $Base 24
        WindowsCompleted = Get-U32 $Map $Base 28
    }
}

function Assert-StackProofRecord {
    param([Parameter(Mandatory)] $StackProof)
    if ($StackProof.Magic -ne $script:StackProofMagic) {
        throw "RF pipeline stack proof magic mismatch: $(Format-Hex32 $StackProof.Magic)."
    }
    if ($StackProof.Version -ne $script:StackProofVersion) {
        throw "RF pipeline stack proof version is $($StackProof.Version); expected $($script:StackProofVersion)."
    }
    if ($StackProof.CompletionMagic -ne $script:StackProofDoneMagic) {
        throw "RF pipeline stack proof is incomplete: completion=$(Format-Hex32 $StackProof.CompletionMagic)."
    }
    if (($StackProof.StackBytes -ne 12288) -or
        ($StackProof.UsedHighWaterBytes -eq 0) -or
        ($StackProof.FreeLowWaterBytes -eq 0) -or
        ($StackProof.UsedHighWaterBytes -gt $StackProof.StackBytes) -or
        ($StackProof.FreeLowWaterBytes -gt $StackProof.StackBytes) -or
        (($StackProof.UsedHighWaterBytes + $StackProof.FreeLowWaterBytes) -ne
         $StackProof.StackBytes) -or
        ($StackProof.Observations -eq 0)) {
        throw 'RF pipeline stack proof contains an invalid or exhausted watermark.'
    }
}

function Invoke-JLinkRead {
    param(
        [Parameter(Mandatory)] [string] $Executable,
        [Parameter(Mandatory)] [string] $Serial,
        [Parameter(Mandatory)] [uint32] $Address,
        [Parameter(Mandatory)] [uint32] $StackAddress
    )
    $commands = @(
        'speed 4000',
        'speed 4000',
        'halt',
        ("mem32 {0} {1}" -f (Format-Hex32 $Address), $script:ProofWords),
        ("mem32 {0} {1}" -f (Format-Hex32 $StackAddress), $script:StackProofWords),
        'go',
        'exit'
    )
    $inputText = ($commands -join "`r`n") + "`r`n"
    $arguments = @(
        '-NoGui', '1',
        '-SelectEmuBySN', $Serial,
        '-Device', 'R7KA8P1KF_CPU0',
        '-If', 'SWD',
        '-Speed', '4000',
        '-AutoConnect', '1'
    )
    $lines = New-Object System.Collections.Generic.List[string]
    $saved = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        (($inputText | & $Executable @arguments 2>&1) | ForEach-Object {
            [void] $lines.Add([string] $_)
        })
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $saved
    }
    $text = $lines -join "`n"
    if ($exitCode -ne 0) { throw "J-Link exited with code $exitCode.`n$text" }
    if ($text -match '(?im)Cannot connect|Failed to connect|No emulator connected|Could not find.*J-Link|ERROR:') {
        throw "J-Link connection/read evidence contains a failure.`n$text"
    }
    return $text
}

function Set-FakeWord {
    param([hashtable] $Map, [uint32] $Base, [int] $Offset, [uint32] $Value)
    $Map[('{0:X8}' -f (Add-Address $Base $Offset))] = $Value
}

function Set-FakeSeries {
    param([hashtable] $Map, [uint32] $Base, [int] $Offset, [uint32[]] $Samples)
    for ($i = 0; $i -lt $script:ProofRuns; $i++) {
        Set-FakeWord $Map $Base ($Offset + (4 * $i)) $Samples[$i]
    }
    $sorted = @($Samples | Sort-Object { [uint64] $_ })
    Set-FakeWord $Map $Base ($Offset + 20) $sorted[0]
    Set-FakeWord $Map $Base ($Offset + 24) $sorted[2]
    Set-FakeWord $Map $Base ($Offset + 28) $sorted[4]
}

function Invoke-SelfTest {
    $base = [uint32] 0x22010000
    $stackBase = [uint32] 0x22011000
    $map = @{}
    $header = @(
        $script:ProofMagic, 1, 2, $script:ProofDoneMagic, $script:RequiredFlags,
        1000000000, 1, 5, 590336, 1, 12, 1024, 512, 1152, 0, 10, 256,
        [uint32] 0x12345678, 301, 9999, 0, 0, 0, 0
    )
    for ($i = 0; $i -lt $header.Count; $i++) {
        Set-FakeWord $map $base (4 * $i) $header[$i]
    }
    [uint32[]] $window = 100, 102, 101, 104, 103
    [uint32[]] $fft = 1000, 1010, 1005, 1020, 1015
    [uint32[]] $reduce = 200, 204, 202, 208, 206
    [uint32[]] $pool = 50, 52, 51, 54, 53
    [uint32[]] $hot = for ($i = 0; $i -lt 5; $i++) {
        $window[$i] + $fft[$i] + $reduce[$i] + $pool[$i]
    }
    [uint32[]] $ingest = 300, 304, 302, 308, 306
    [uint32[]] $full = for ($i = 0; $i -lt 5; $i++) { $hot[$i] + $ingest[$i] }
    Set-FakeSeries $map $base $script:Series.FullWindow $full
    Set-FakeSeries $map $base $script:Series.StftHot $hot
    Set-FakeSeries $map $base $script:Series.WindowApply $window
    Set-FakeSeries $map $base $script:Series.Fft $fft
    Set-FakeSeries $map $base $script:Series.PowerReduce $reduce
    Set-FakeSeries $map $base $script:Series.PoolAndQuantize $pool
    Set-FakeSeries $map $base $script:Series.IngestAndSchedule $ingest
    $proof = Get-ProofRecord $map $base
    Assert-ProofRecord $proof
    if ($proof.Series.FullWindow.MedianCycles -ne 1672) {
        throw 'Self-test median parsing failed.'
    }
    if ($proof.Series.FullWindow.MedianMs -ne 0.001672) {
        throw 'Self-test cycle-to-ms conversion failed.'
    }
    [uint32[]] $stackWords = @(
        $script:StackProofMagic, $script:StackProofVersion,
        $script:StackProofDoneMagic, 12288, 6144, 6144, 2, 1
    )
    for ($i = 0; $i -lt $stackWords.Count; $i++) {
        Set-FakeWord $map $stackBase (4 * $i) $stackWords[$i]
    }
    $stackProof = Get-StackProofRecord $map $stackBase
    Assert-StackProofRecord $stackProof
    Write-Output 'Self-test passed: 320-byte STFT ABI, 32-byte stack ABI, raw samples, summaries, stage sums and timing conversion.'
}

function Write-HumanReport {
    param([Parameter(Mandatory)] $Report)
    $proof = $Report.Proof
    Write-Output 'RA8P1 CPU0 synthetic STFT proof (read-only SWD)'
    Write-Output ("Probe={0} Target={1}" -f $Report.ProbeSerial, $Report.Target)
    Write-Output ("CPU0 ELF: {0}" -f $Report.Elf.Path)
    Write-Output ("SHA256={0} symbol={1} address={2} bytes={3}" -f
        $Report.Elf.Sha256, $Report.Symbol.Name,
        (Format-Hex32 $Report.Symbol.Address), $Report.Symbol.Size)
    Write-Output ("status=PASS warmup={0} runs={1} samples={2} FFT/hop/frames={3}/{4}/{5}" -f
        $proof.WarmupRuns, $proof.MeasuredRuns, $proof.ComplexSamples,
        $proof.FftSize, $proof.HopSize, $proof.StftFrames)
    foreach ($entry in $script:Series.GetEnumerator()) {
        $series = $proof.Series.($entry.Key)
        Write-Output ("{0}: raw_ms=[{1}] min/median/max={2}/{3}/{4} ms" -f
            $entry.Key, ($series.SamplesMs -join ', '),
            $series.MinimumMs, $series.MedianMs, $series.MaximumMs)
    }
    Write-Output ("checksum={0} peak_bin={1} peak_power={2} CFSR/HFSR={3}/{4}" -f
        (Format-Hex32 $proof.Checksum), $proof.PeakBin, $proof.PeakPower,
        (Format-Hex32 $proof.Cfsr), (Format-Hex32 $proof.Hfsr))
    Write-Output ("rfpipe_stack: symbol={0} address={1} bytes={2} used_high_water={3} free_low_water={4} observations={5} windows={6}" -f
        $Report.StackSymbol.Name, (Format-Hex32 $Report.StackSymbol.Address),
        $Report.StackSymbol.Size, $Report.StackProof.UsedHighWaterBytes,
        $Report.StackProof.FreeLowWaterBytes, $Report.StackProof.Observations,
        $Report.StackProof.WindowsCompleted)
    Write-Output 'Boundary: synthetic CPU0 compute only; input generation, Ethernet, CRC, NPU and IPC are excluded.'
    Write-Output 'The ELF hash binds symbol/ABI interpretation; flash/download evidence must separately bind this ELF to the board.'
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
$symbol = Get-ExactSymbol $nm $elfPath $script:SymbolName $script:ProofBytes
$stackSymbol = Get-ExactSymbol $nm $elfPath $script:StackSymbolName $script:StackProofBytes
$jlinkText = Invoke-JLinkRead $jlink $serial $symbol.Address $stackSymbol.Address
$map = Get-Mem32Map $jlinkText
$proof = Get-ProofRecord $map $symbol.Address
Assert-ProofRecord $proof
$stackProof = Get-StackProofRecord $map $stackSymbol.Address
Assert-StackProofRecord $stackProof

$report = [ordered]@{
    Tool = 'ra8p1-stft-proof'
    ToolVersion = '1.1'
    TimestampUtc = (Get-Date).ToUniversalTime().ToString('o')
    ProbeSerial = $serial
    Target = 'R7KA8P1KF_CPU0'
    Elf = $elf
    Symbol = $symbol
    Proof = $proof
    StackSymbol = $stackSymbol
    StackProof = $stackProof
    EvidenceBoundary = 'Synthetic CPU0 compute only; input generation, Ethernet, CRC, NPU and IPC excluded. ELF hash binds ABI interpretation, while flash evidence separately binds the ELF to the board.'
}

if ($Json) {
    $report | ConvertTo-Json -Depth 8
}
else {
    Write-HumanReport $report
}

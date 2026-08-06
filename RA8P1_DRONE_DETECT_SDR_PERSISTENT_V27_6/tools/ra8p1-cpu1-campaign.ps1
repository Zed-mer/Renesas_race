<#
.SYNOPSIS
Starts and inspects CPU1-owned SDR campaigns through J-Link/SWD.

.DESCRIPTION
The exact CPU1 ELF supplies the addresses of the CPU1 request and proof
objects. J-Link always connects through R7KA8P1KF_CPU0 on probe 1082495494;
the script never attaches CPU1 and never writes the CPU0 command mailbox.
CPU1 consumes the request and sends the normal shared-memory IPC command to
CPU0, which remains the only owner of SDRC/UDP/5004.
#>
[CmdletBinding()]
param(
    [ValidateSet('ReadStatus', 'Stop', 'Single', 'FourOverlap', 'FourSerial')]
    [string] $Action = 'ReadStatus',
    [string] $Cpu1Elf,
    [string] $ProbeSerial = '1082495494',
    [string] $JLinkExe,
    [string] $NmExe,
    [ValidateRange(0, 3)] [uint32] $CenterIndex = 0,
    [ValidateRange(1, 100000)] [uint32] $Iterations = 1,
    [ValidateRange(1, 940)] [uint32] $PayloadMbps = 800,
    [ValidateRange(0, 15)] [uint32] $FaultFlags = 0,
    [uint32] $RequestId = 0,
    [switch] $DryRun,
    [switch] $Json,
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ExpectedProbe = '1082495494'
$script:Cpu0Target = 'R7KA8P1KF_CPU0'
$script:RequestMagic = [uint32]0x51525043
$script:ProofMagic = [uint32]0x46525043
$script:Version = [uint16]1
$script:RequestBytes = 64
$script:ProofBytes = 128
$script:CompleteMagic = [uint32]0x454E4F44
$script:FailureMagic = [uint32]0x4C494146
$script:Symbols = [ordered]@{
    Request = [pscustomobject]@{
        Name = 'g_cpu1_campaign_control'
        Bytes = $script:RequestBytes
    }
    Proof = [pscustomobject]@{
        Name = 'g_cpu1_campaign_proof'
        Bytes = $script:ProofBytes
    }
}
$script:Modes = @{
    Stop = [uint32]1
    Single = [uint32]2
    FourOverlap = [uint32]3
    FourSerial = [uint32]4
}
$script:StateNames = @{
    0 = 'UNINITIALIZED'; 1 = 'READY'; 2 = 'STOPPING'; 3 = 'ARMING'
    4 = 'RUNNING'; 5 = 'RETRY_WAIT'; 6 = 'COMPLETE'; 7 = 'STOPPED'
    8 = 'ERROR'
}
$script:ModeNames = @{
    0 = 'NONE'; 1 = 'STOP'; 2 = 'SINGLE'; 3 = 'FOUR_OVERLAP'
    4 = 'FOUR_SERIAL'
}

function Format-Hex32 {
    param([uint32] $Value)
    return ('0x{0:X8}' -f $Value)
}

function Add-Address {
    param([uint32] $Base, [int] $Offset)
    return [uint32]([uint64]$Base + [uint64]$Offset)
}

function Read-HostConfig {
    $path = Join-Path $HOME '.codex\ra8p1.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return @{} }
    try {
        $object = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
        $config = @{}
        foreach ($property in $object.PSObject.Properties) {
            $config[$property.Name] = [string]$property.Value
        }
        return $config
    }
    catch {
        throw "Could not read host configuration $path`: $($_.Exception.Message)"
    }
}

function Resolve-Probe {
    param([string] $Requested)
    $candidate = $Requested.Trim().TrimStart('0')
    if ($candidate -ne $script:ExpectedProbe) {
        throw "This campaign is pinned to probe $($script:ExpectedProbe); received '$Requested'."
    }
    return $candidate
}

function Resolve-JLink {
    param([string] $Requested, [hashtable] $Config)
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($Requested) { [void]$candidates.Add($Requested) }
    if ($env:RA8P1_JLINK_ROOT) {
        [void]$candidates.Add((Join-Path $env:RA8P1_JLINK_ROOT 'JLink.exe'))
    }
    if ($Config['JLinkRoot']) {
        [void]$candidates.Add((Join-Path $Config['JLinkRoot'] 'JLink.exe'))
    }
    $command = Get-Command JLink.exe -ErrorAction SilentlyContinue
    if ($command) { [void]$candidates.Add($command.Source) }
    [void]$candidates.Add((Join-Path $HOME 'SEGGER\JLink_V958\JLink.exe'))
    [void]$candidates.Add('C:\Program Files\SEGGER\JLink\JLink.exe')
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
    if ($Requested) { [void]$candidates.Add($Requested) }
    if ($env:RA8P1_E2_ROOT) {
        [void]$candidates.Add((Join-Path $env:RA8P1_E2_ROOT 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe'))
    }
    if ($Config['E2Root']) {
        [void]$candidates.Add((Join-Path $Config['E2Root'] 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe'))
    }
    $command = Get-Command arm-none-eabi-nm.exe -ErrorAction SilentlyContinue
    if ($command) { [void]$candidates.Add($command.Source) }
    [void]$candidates.Add('C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe')
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'arm-none-eabi-nm.exe was not found. Pass -NmExe or set RA8P1_E2_ROOT.'
}

function Resolve-ExactCpu1Elf {
    param([string] $Requested)
    if (-not $Requested) {
        throw '-Cpu1Elf is required. Automatic latest-ELF selection is intentionally disabled.'
    }
    $path = [IO.Path]::GetFullPath($Requested)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "CPU1 ELF does not exist: $path"
    }
    return (Resolve-Path -LiteralPath $path).Path
}

function Get-ElfRecord {
    param([Parameter(Mandatory)] [string] $Path)
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        Path = $item.FullName
        Size = [uint64]$item.Length
        LastWriteTimeUtc = $item.LastWriteTimeUtc.ToString('o')
        Sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
    }
}

function Get-CampaignSymbols {
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
        $match = [regex]::Match([string]$line,
            '^\s*(?<address>[0-9A-Fa-f]+)\s+(?<size>[0-9A-Fa-f]+)\s+(?<type>\S)\s+(?<name>\S+)\s*$')
        if (-not $match.Success) { continue }
        foreach ($expected in $script:Symbols.Values) {
            if ($match.Groups['name'].Value -eq $expected.Name) {
                $found[$expected.Name] = [pscustomobject]@{
                    Name = $expected.Name
                    Address = [uint32]([Convert]::ToUInt64($match.Groups['address'].Value, 16))
                    Size = [uint32]([Convert]::ToUInt64($match.Groups['size'].Value, 16))
                    Type = $match.Groups['type'].Value
                }
            }
        }
    }
    $result = [ordered]@{}
    foreach ($key in $script:Symbols.Keys) {
        $expected = $script:Symbols[$key]
        if (-not $found.ContainsKey($expected.Name)) {
            throw "Required CPU1 ELF symbol was not found: $($expected.Name)"
        }
        if ($found[$expected.Name].Size -ne $expected.Bytes) {
            throw "CPU1 ELF symbol $($expected.Name) is $($found[$expected.Name].Size) bytes; expected exactly $($expected.Bytes)."
        }
        if (($found[$expected.Name].Address -band 31) -ne 0) {
            throw "CPU1 ELF symbol $($expected.Name) is not 32-byte aligned: $(Format-Hex32 $found[$expected.Name].Address)."
        }
        if ($found[$expected.Name].Type -notmatch '^[BbDd]$') {
            throw "CPU1 ELF symbol $($expected.Name) has non-data type '$($found[$expected.Name].Type)'."
        }
        $result[$key] = $found[$expected.Name]
    }
    return $result
}

function New-RequestWords {
    param(
        [Parameter(Mandatory)] [uint32] $Mode,
        [Parameter(Mandatory)] [uint32] $Id,
        [Parameter(Mandatory)] [uint32] $Sequence,
        [uint32] $Center,
        [uint32] $Count,
        [uint32] $RateMbps,
        [uint32] $Faults
    )
    $words = [uint32[]](0..15 | ForEach-Object { 0 })
    $words[0] = $Sequence
    $words[1] = $script:RequestMagic
    $words[2] = [uint32](([uint32]$script:RequestBytes -shl 16) -bor $script:Version)
    $words[3] = $Id
    $words[4] = $Mode
    $words[5] = $Center
    $words[6] = $Count
    $words[7] = [uint32]([uint64]$RateMbps * 1000)
    $words[8] = $Faults
    $words[9] = 0
    $words[15] = $Sequence
    return $words
}

function New-RequestIdentity {
    param([uint32] $Requested)
    $id = $Requested
    if ($id -eq 0) {
        $id = [uint32]([uint64][DateTime]::UtcNow.Ticks -band [uint64]4294967295)
        if ($id -eq 0) { $id = 1 }
    }
    # Windows PowerShell 5 parses 0xFFFFFFFE as signed -2 before the cast.
    $sequence = [uint32]((([uint64]$id * [uint64]2) + [uint64]2) -band [uint64]4294967294)
    if ($sequence -eq 0) { $sequence = 2 }
    return [pscustomobject]@{ RequestId = $id; Sequence = $sequence }
}

function New-JLinkCommands {
    param(
        [Parameter(Mandatory)] $ResolvedSymbols,
        [uint32[]] $RequestWords
    )
    $commands = New-Object System.Collections.Generic.List[string]
    foreach ($command in @('speed 4000', 'halt')) {
        [void]$commands.Add($command)
    }
    if ($null -ne $RequestWords) {
        $base = [uint32]$ResolvedSymbols.Request.Address
        $even = [uint32]$RequestWords[0]
        [void]$commands.Add(('w4 {0}, {1}' -f (Format-Hex32 $base),
                                                (Format-Hex32 ($even -bor 1))))
        for ($index = 1; $index -lt 15; $index++) {
            [void]$commands.Add(('w4 {0}, {1}' -f
                (Format-Hex32 (Add-Address $base (4 * $index))),
                (Format-Hex32 $RequestWords[$index])))
        }
        [void]$commands.Add(('w4 {0}, {1}' -f
            (Format-Hex32 (Add-Address $base 60)), (Format-Hex32 $even)))
        [void]$commands.Add(('w4 {0}, {1}' -f (Format-Hex32 $base),
                                                (Format-Hex32 $even)))
    }
    [void]$commands.Add(('mem32 {0} 16' -f
        (Format-Hex32 ([uint32]$ResolvedSymbols.Request.Address))))
    [void]$commands.Add(('mem32 {0} 32' -f
        (Format-Hex32 ([uint32]$ResolvedSymbols.Proof.Address))))
    [void]$commands.Add('go')
    [void]$commands.Add('exit')
    return $commands
}

function Invoke-JLink {
    param(
        [Parameter(Mandatory)] [string] $Executable,
        [Parameter(Mandatory)] [string[]] $Commands,
        [Parameter(Mandatory)] [string] $Serial
    )
    $arguments = @(
        '-NoGui', '1', '-SelectEmuBySN', $Serial,
        '-Device', $script:Cpu0Target, '-If', 'SWD', '-Speed', '4000',
        '-AutoConnect', '1'
    )
    $inputText = ($Commands -join "`r`n") + "`r`n"
    $lines = New-Object System.Collections.Generic.List[string]
    $saved = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        (($inputText | & $Executable @arguments 2>&1) | ForEach-Object {
            [void]$lines.Add([string]$_)
        })
        $exitCode = $LASTEXITCODE
    }
    catch {
        [void]$lines.Add($_.Exception.Message)
        $exitCode = -1
    }
    finally {
        $ErrorActionPreference = $saved
    }
    return [pscustomobject]@{ Text = ($lines -join "`n"); ExitCode = $exitCode }
}

function Assert-JLinkEvidence {
    param([string] $Text, [int] $ExitCode, [string] $Serial)
    if ($ExitCode -ne 0) {
        throw "J-Link Commander failed with exit code $ExitCode.`n$Text"
    }
    if ($Text -match 'Cannot connect|Failed to connect|Could not find emulator|No emulator connected|Target connection not established|CPU is not halted') {
        throw "J-Link connection failed.`n$Text"
    }
    $serialPattern = 'S/N:\s*0*' + [regex]::Escape($Serial) + '\s*$'
    if ($Text -notmatch "(?m)^\s*(?:J-Link>\s*)?$serialPattern") {
        throw "J-Link output did not prove probe serial $Serial."
    }
    if ($Text -notmatch '(?m)^\s*(?:J-Link>\s*)?Device\s+"R7KA8P1KF_CPU0"\s+selected\.') {
        throw 'J-Link output did not prove the CPU0 target.'
    }
    if ($Text -notmatch 'Cortex-M85 identified') {
        throw 'J-Link output did not prove the CPU0 Cortex-M85.'
    }
    $haltIndex = $Text.IndexOf('PC =', [StringComparison]::OrdinalIgnoreCase)
    foreach ($match in [regex]::Matches($Text, '(?im)^.*Unknown command.*$')) {
        if (($haltIndex -lt 0) -or ($match.Index -gt $haltIndex)) {
            throw "J-Link rejected a campaign command after CPU0 halt.`n$Text"
        }
    }
}

function Get-Mem32Map {
    param([string] $Text)
    $map = @{}
    foreach ($line in ($Text -split "`r?`n")) {
        $match = [regex]::Match($line,
            '^\s*(?:J-Link>\s*)?([0-9A-Fa-f]{8})\s*=\s*(.+)$')
        if (-not $match.Success) { continue }
        $start = [uint32]([Convert]::ToUInt64($match.Groups[1].Value, 16))
        $words = [regex]::Matches($match.Groups[2].Value,
            '(?<![0-9A-Fa-f])([0-9A-Fa-f]{8})(?![0-9A-Fa-f])')
        for ($index = 0; $index -lt $words.Count; $index++) {
            $address = Add-Address $start (4 * $index)
            $map[('{0:X8}' -f $address)] =
                [uint32]([Convert]::ToUInt64($words[$index].Groups[1].Value, 16))
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
    return [uint32]$Map[$key]
}

function Assert-RequestReadback {
    param([hashtable] $Map, $Symbol, [uint32[]] $Expected)
    for ($index = 0; $index -lt $Expected.Count; $index++) {
        $actual = Get-U32 $Map ([uint32]$Symbol.Address) (4 * $index)
        if ($actual -ne $Expected[$index]) {
            throw "Campaign request readback mismatch at word $index`: expected $(Format-Hex32 $Expected[$index]), got $(Format-Hex32 $actual)."
        }
    }
}

function Get-Proof {
    param([hashtable] $Map, $Symbol)
    $base = [uint32]$Symbol.Address
    $begin = Get-U32 $Map $base 0
    $end = Get-U32 $Map $base 124
    if (($begin -eq 0) -or (($begin -band 1) -ne 0) -or ($begin -ne $end)) {
        throw "CPU1 campaign proof seqlock is unstable: begin=$(Format-Hex32 $begin), end=$(Format-Hex32 $end)."
    }
    $magic = Get-U32 $Map $base 4
    $versionSize = Get-U32 $Map $base 8
    if ($magic -ne $script:ProofMagic -or
        (($versionSize -band 0xFFFF) -ne $script:Version) -or
        (($versionSize -shr 16) -ne $script:ProofBytes)) {
        throw "CPU1 campaign proof header mismatch: magic=$(Format-Hex32 $magic), version/size=$(Format-Hex32 $versionSize)."
    }
    $state = Get-U32 $Map $base 20
    $mode = Get-U32 $Map $base 24
    $terminal = Get-U32 $Map $base 120
    return [ordered]@{
        Sequence = $begin
        RequestId = Get-U32 $Map $base 12
        RequestSequence = Get-U32 $Map $base 16
        State = $state
        StateName = if ($script:StateNames.ContainsKey([int]$state)) { $script:StateNames[[int]$state] } else { 'UNKNOWN' }
        Mode = $mode
        ModeName = if ($script:ModeNames.ContainsKey([int]$mode)) { $script:ModeNames[[int]$mode] } else { 'UNKNOWN' }
        ConfiguredCenterIndex = Get-U32 $Map $base 28
        IterationsRequested = Get-U32 $Map $base 32
        IterationsCompleted = Get-U32 $Map $base 36
        WindowsExpected = Get-U32 $Map $base 40
        WindowsVisible = Get-U32 $Map $base 44
        NextCenterIndex = Get-U32 $Map $base 48
        ActiveCenterIndex = Get-U32 $Map $base 52
        TargetPayloadMbpsX1000 = Get-U32 $Map $base 56
        TestFaultFlags = Get-U32 $Map $base 60
        CampaignFlags = Get-U32 $Map $base 64
        LastSessionId = Get-U32 $Map $base 68
        LastWindowSequence = Get-U32 $Map $base 72
        LastResultCenterIndex = Get-U32 $Map $base 76
        LastCommandSequence = Get-U32 $Map $base 80
        LastCommandStatus = Get-U32 $Map $base 84
        LastCommandReason = Get-U32 $Map $base 88
        LastAppliedSessionId = Get-U32 $Map $base 92
        CommandSendRetries = Get-U32 $Map $base 96
        BusyRetries = Get-U32 $Map $base 100
        RejectedRequests = Get-U32 $Map $base 104
        DuplicateRequests = Get-U32 $Map $base 108
        UnexpectedResults = Get-U32 $Map $base 112
        LastError = Get-U32 $Map $base 116
        TerminalMagic = Format-Hex32 $terminal
        Complete = ($terminal -eq $script:CompleteMagic)
        Failed = ($terminal -eq $script:FailureMagic)
    }
}

function Assert-SelfTest {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw "Self-test failed: $Message" }
}

function Invoke-SelfTest {
    $symbols = [ordered]@{
        Request = [pscustomobject]@{ Name = 'g_cpu1_campaign_control'; Address = [uint32]0x22010000; Size = 64; Type = 'D' }
        Proof = [pscustomobject]@{ Name = 'g_cpu1_campaign_proof'; Address = [uint32]0x22010100; Size = 128; Type = 'D' }
    }
    $words = New-RequestWords 3 77 156 0 10 800 1
    Assert-SelfTest ($words.Count -eq 16) 'request is 64 bytes'
    Assert-SelfTest ($words[1] -eq $script:RequestMagic) 'request magic'
    Assert-SelfTest (($words[2] -band 0xFFFF) -eq 1) 'request version'
    Assert-SelfTest (($words[2] -shr 16) -eq 64) 'request size'
    Assert-SelfTest ($words[4] -eq 3 -and $words[6] -eq 10) 'four-overlap rounds'
    Assert-SelfTest ($words[7] -eq 800000 -and $words[8] -eq 1) 'rate/fault fields'
    Assert-SelfTest ($words[0] -eq $words[15] -and (($words[0] -band 1) -eq 0)) 'request seqlock'
    $commands = @(New-JLinkCommands $symbols $words)
    Assert-SelfTest ($commands[1] -eq 'halt') 'CPU0 halt precedes writes'
    Assert-SelfTest ($commands[2] -match '^w4 0x22010000, 0x[0-9A-F]{8}$') 'odd begin write exists'
    Assert-SelfTest ($commands[17] -eq 'w4 0x2201003C, 0x0000009C') 'end is committed before begin'
    Assert-SelfTest ($commands[18] -eq 'w4 0x22010000, 0x0000009C') 'even begin commits request'
    Assert-SelfTest (($commands -join "`n") -notmatch 'R7KA8P1KF_CPU1') 'no direct CPU1 target'
    Assert-SelfTest ($script:Cpu0Target -eq 'R7KA8P1KF_CPU0') 'CPU0 target is fixed'
    Assert-SelfTest ($script:ExpectedProbe -eq '1082495494') 'probe is fixed'
    Write-Output 'Self-test passed: fixed ABI, seqlock write order, rate/fault encoding, CPU0-only target and probe pinning.'
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

$serial = Resolve-Probe $ProbeSerial
$config = Read-HostConfig
$nm = Resolve-Nm $NmExe $config
$elfPath = Resolve-ExactCpu1Elf $Cpu1Elf
$elf = Get-ElfRecord $elfPath
$symbols = Get-CampaignSymbols $nm $elfPath
$requestWords = $null
$identity = $null
if ($Action -ne 'ReadStatus') {
    $identity = New-RequestIdentity $RequestId
    $mode = [uint32]$script:Modes[$Action]
    $count = if ($Action -eq 'Stop') { [uint32]0 } else { $Iterations }
    $center = if ($Action -eq 'Single') { $CenterIndex } else { [uint32]0 }
    $requestWords = New-RequestWords $mode $identity.RequestId $identity.Sequence `
                                      $center $count $PayloadMbps $FaultFlags
}
$commands = @(New-JLinkCommands $symbols $requestWords)
$symbolReport = [ordered]@{}
foreach ($key in $symbols.Keys) {
    $symbolReport[$key] = [ordered]@{
        Name = $symbols[$key].Name
        Address = Format-Hex32 ([uint32]$symbols[$key].Address)
        Size = $symbols[$key].Size
        Type = $symbols[$key].Type
    }
}

if ($DryRun) {
    $report = [ordered]@{
        Tool = 'ra8p1-cpu1-campaign'
        ToolVersion = '1.0'
        DryRun = $true
        ProbeSerial = $serial
        Target = $script:Cpu0Target
        Action = $Action
        Elf = $elf
        Symbols = $symbolReport
        RequestId = if ($identity) { $identity.RequestId } else { $null }
        Commands = $commands
        EvidenceBoundary = 'No J-Link connection or target write was performed.'
    }
    if ($Json) { $report | ConvertTo-Json -Depth 7 }
    else { $report | Format-List }
    exit 0
}

$jlink = Resolve-JLink $JLinkExe $config
$invocation = Invoke-JLink $jlink $commands $serial
Assert-JLinkEvidence $invocation.Text $invocation.ExitCode $serial
$memory = Get-Mem32Map $invocation.Text
if ($null -ne $requestWords) {
    Assert-RequestReadback $memory $symbols.Request $requestWords
}
$proof = Get-Proof $memory $symbols.Proof
$report = [ordered]@{
    Tool = 'ra8p1-cpu1-campaign'
    ToolVersion = '1.0'
    TimestampUtc = (Get-Date).ToUniversalTime().ToString('o')
    ProbeSerial = $serial
    Target = $script:Cpu0Target
    Action = $Action
    JLinkExe = $jlink
    NmExe = $nm
    Elf = $elf
    Symbols = $symbolReport
    RequestId = if ($identity) { $identity.RequestId } else { $null }
    Proof = $proof
    EvidenceBoundary = 'The exact CPU1 ELF binds symbol interpretation. J-Link used the CPU0 target; CPU1 must subsequently prove IPC command/result progress.'
}
if ($Json) {
    $report | ConvertTo-Json -Depth 7
}
else {
    Write-Output ("CPU1 campaign: action={0}, request={1}, state={2}, mode={3}" -f
        $Action, $proof.RequestId, $proof.StateName, $proof.ModeName)
    Write-Output ("  progress: windows={0}/{1}, iterations={2}/{3}, next_center={4}" -f
        $proof.WindowsVisible, $proof.WindowsExpected,
        $proof.IterationsCompleted, $proof.IterationsRequested,
        $proof.NextCenterIndex)
    Write-Output ("  IPC command: sequence={0}, status={1}, reason={2}, applied_session={3}" -f
        $proof.LastCommandSequence, $proof.LastCommandStatus,
        $proof.LastCommandReason, $proof.LastAppliedSessionId)
    Write-Output ("  retries: send={0}, busy={1}; rejected_requests={2}; unexpected_results={3}; error={4}" -f
        $proof.CommandSendRetries, $proof.BusyRetries,
        $proof.RejectedRequests, $proof.UnexpectedResults, $proof.LastError)
    Write-Output ("  CPU1 ELF SHA-256: {0}" -f $elf.Sha256)
}

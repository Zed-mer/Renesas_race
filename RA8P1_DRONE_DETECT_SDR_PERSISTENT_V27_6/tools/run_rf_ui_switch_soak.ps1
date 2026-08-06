[CmdletBinding()]
param(
    [string] $Elf = (Join-Path $PSScriptRoot '..\cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf'),
    [ValidatePattern('^[0-9]{6,20}$')] [string] $ProbeSerial = '1082495494',
    [ValidateRange(1, 10000)] [uint32] $Switches = 100,
    [ValidateRange(1, 10)] [int] $PollSeconds = 2,
    [ValidateRange(10, 1800)] [int] $TimeoutSeconds = 600,
    [string] $JLinkExe = 'C:\Program Files\SEGGER\JLink_V956\JLink.exe',
    [string] $NmExe = 'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe',
    [string] $DisplayDiagScript = (Join-Path $PSScriptRoot 'read_display_diag.ps1'),
    [string] $RfUiDiagScript = (Join-Path $PSScriptRoot 'read_rf_ui_diag.ps1'),
    [string] $OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$fieldNames = @(
    'magic', 'version', 'command_generation', 'active_generation',
    'requested_switches', 'completed_switches', 'running', 'errors',
    'next_channel', 'last_requested_channel'
)
$expectedMagic = [uint32]0x534F414B

foreach($path in @($Elf, $JLinkExe, $NmExe, $DisplayDiagScript,
                    $RfUiDiagScript)) {
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required switch-soak input was not found: $path"
    }
}

$Elf = (Resolve-Path -LiteralPath $Elf).Path
$JLinkExe = (Resolve-Path -LiteralPath $JLinkExe).Path
$NmExe = (Resolve-Path -LiteralPath $NmExe).Path
$DisplayDiagScript = (Resolve-Path -LiteralPath $DisplayDiagScript).Path
$RfUiDiagScript = (Resolve-Path -LiteralPath $RfUiDiagScript).Path
$ProbeSerial = $ProbeSerial.TrimStart('0')

$nmLine = @(& $NmExe -S -C $Elf) |
    Where-Object { $_ -match '\bg_rf_ui_channel_soak$' } |
    Select-Object -First 1
if(-not $nmLine -or
   $nmLine -notmatch '^(?<address>[0-9A-Fa-f]+)\s+(?<size>[0-9A-Fa-f]+)\s+\S\s+g_rf_ui_channel_soak$') {
    throw 'g_rf_ui_channel_soak was not found in the selected CPU1 ELF.'
}
$address = [Convert]::ToUInt32($Matches.address, 16)
$size = [Convert]::ToUInt32($Matches.size, 16)
if($size -ne ($fieldNames.Count * 4)) {
    throw "Unexpected channel-soak size: ELF=$size expected=$($fieldNames.Count * 4)."
}

function Invoke-JLinkCommands {
    param([string[]] $Commands)

    $arguments = @(
        '-NoGui', '1', '-AutoConnect', '1', '-Device', 'R7KA8P1KF_CPU0',
        '-If', 'SWD', '-Speed', '4000', '-SelectEmuBySN', $ProbeSerial
    )
    $raw = @($Commands + @('exit') | & $JLinkExe @arguments 2>&1)
    $text = $raw -join "`n"
    if(($LASTEXITCODE -ne 0) -or
       ($text -notmatch "S/N:\s*$([regex]::Escape($ProbeSerial))") -or
       ($text -notmatch 'Device "R7KA8P1KF_CPU0" selected') -or
       ($text -match '(?i)cannot connect|connection failed|unknown command|write failed')) {
        throw "J-Link switch-soak access failed.`n$text"
    }
    return ,$raw
}

function Read-SoakSnapshot {
    $addressHex = '0x{0:X8}' -f $address
    $countHex = '0x{0:X}' -f $fieldNames.Count
    $raw = Invoke-JLinkCommands -Commands @("mem32 $addressHex, $countHex")
    $words = New-Object 'uint32[]' $fieldNames.Count
    $seen = New-Object 'bool[]' $fieldNames.Count

    foreach($line in $raw) {
        $text = [string]$line
        if($text -notmatch '^(?:J-Link>)?(?<lineAddress>[0-9A-Fa-f]{8})\s*=\s*(?<values>.*)$') {
            continue
        }
        $lineAddress = [Convert]::ToUInt32($Matches.lineAddress, 16)
        if($lineAddress -lt $address -or $lineAddress -ge ($address + $size)) {
            continue
        }
        $tokens = @($Matches.values -split '\s+' |
            Where-Object { $_ -match '^[0-9A-Fa-f]{8}$' })
        $start = [int](($lineAddress - $address) / 4)
        for($offset = 0; $offset -lt $tokens.Count; ++$offset) {
            $index = $start + $offset
            if($index -ge $words.Length) { break }
            $words[$index] = [Convert]::ToUInt32($tokens[$offset], 16)
            $seen[$index] = $true
        }
    }
    if($seen -contains $false) {
        throw 'J-Link did not return the complete channel-soak object.'
    }

    $snapshot = [ordered]@{}
    for($index = 0; $index -lt $fieldNames.Count; ++$index) {
        $snapshot[$fieldNames[$index]] = [uint32]$words[$index]
    }
    if($snapshot.magic -ne $expectedMagic -or $snapshot.version -ne 1U) {
        throw ('Unexpected channel-soak identity: magic=0x{0:X8}, version={1}' -f
               $snapshot.magic, $snapshot.version)
    }
    return $snapshot
}

function Read-JsonScript {
    param([string] $Path)
    $json = @(& $Path -ProbeSerial $ProbeSerial -Elf $Elf) -join "`n"
    return $json | ConvertFrom-Json
}

$beforeDisplay = Read-JsonScript -Path $DisplayDiagScript
$beforeRfUi = Read-JsonScript -Path $RfUiDiagScript
$initialSoak = Read-SoakSnapshot
$generation = [uint32]($initialSoak.command_generation + 1U)
if($generation -eq 0U) { $generation = 1U }

$requestedAddress = $address + (4U * 4U)
$generationAddress = $address + (2U * 4U)
$writeCommands = @(
    ('w4 0x{0:X8}, 0x{1:X8}' -f $requestedAddress, $Switches),
    ('w4 0x{0:X8}, 0x{1:X8}' -f $generationAddress, $generation)
)
[void](Invoke-JLinkCommands -Commands $writeCommands)

$startedAt = Get-Date
$deadline = $startedAt.AddSeconds($TimeoutSeconds)
$last = Read-SoakSnapshot
while($last.active_generation -ne $generation -or $last.running -ne 0U) {
    if((Get-Date) -ge $deadline) {
        throw "Channel switch soak timed out after $TimeoutSeconds seconds: completed=$($last.completed_switches)/$Switches errors=$($last.errors)."
    }
    Write-Progress -Activity 'Four-channel switch soak' `
        -Status "$($last.completed_switches)/$Switches committed" `
        -PercentComplete ([math]::Min(100, (100.0 * $last.completed_switches / $Switches)))
    Start-Sleep -Seconds $PollSeconds
    $last = Read-SoakSnapshot
}
Write-Progress -Activity 'Four-channel switch soak' -Completed

$afterDisplay = Read-JsonScript -Path $DisplayDiagScript
$afterRfUi = Read-JsonScript -Path $RfUiDiagScript
$underflowDelta = [uint32](
    $afterDisplay.Snapshot.glcdc_underflows -
    $beforeDisplay.Snapshot.glcdc_underflows)
$requestDelta = [uint32](
    $afterRfUi.Snapshot.requests - $beforeRfUi.Snapshot.requests)
$commitDelta = [uint32](
    $afterRfUi.Snapshot.atomic_commits - $beforeRfUi.Snapshot.atomic_commits)
$elapsed = (Get-Date) - $startedAt

$success = $last.active_generation -eq $generation -and
           $last.completed_switches -eq $Switches -and
           $last.errors -eq 0U -and
           $requestDelta -eq $Switches -and
           $commitDelta -eq $Switches -and
           $underflowDelta -eq 0U -and
           $afterDisplay.Snapshot.animation_buffer_errors -eq 0U -and
           $afterDisplay.Snapshot.fatal_status -eq 0U -and
           $afterDisplay.Snapshot.running -eq 1U

$result = [ordered]@{
    CapturedAt = (Get-Date).ToString('o')
    ProbeSerial = $ProbeSerial
    Elf = $Elf
    ElfSha256 = (Get-FileHash -LiteralPath $Elf -Algorithm SHA256).Hash
    SoakAddress = ('0x{0:X8}' -f $address)
    CommandGeneration = $generation
    RequestedSwitches = $Switches
    CompletedSwitches = $last.completed_switches
    SoakErrors = $last.errors
    ElapsedSeconds = [math]::Round($elapsed.TotalSeconds, 3)
    UnderflowBefore = $beforeDisplay.Snapshot.glcdc_underflows
    UnderflowAfter = $afterDisplay.Snapshot.glcdc_underflows
    UnderflowDelta = $underflowDelta
    RequestDelta = $requestDelta
    AtomicCommitDelta = $commitDelta
    BufferChangeErrors = $afterDisplay.Snapshot.animation_buffer_errors
    FatalStatus = $afterDisplay.Snapshot.fatal_status
    DisplayRunning = $afterDisplay.Snapshot.running
    Success = $success
    FinalSoakSnapshot = $last
}
$json = $result | ConvertTo-Json -Depth 5
if($OutputPath) {
    $parent = Split-Path -Parent $OutputPath
    if($parent) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Set-Content -LiteralPath $OutputPath -Value $json -Encoding utf8
}
$json
if(-not $success) { exit 2 }

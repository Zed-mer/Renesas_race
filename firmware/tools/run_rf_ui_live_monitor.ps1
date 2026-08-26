[CmdletBinding()]
param(
    [string] $Elf = (Join-Path $PSScriptRoot '..\cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf'),
    [ValidatePattern('^[0-9]{6,20}$')] [string] $ProbeSerial = '1082495494',
    [ValidateRange(5, 300)] [int] $Seconds = 30,
    [ValidateRange(1, 1000)] [uint32] $LineEventsPerSecond = 50,
    [string] $JLinkExe = 'C:\Program Files\SEGGER\JLink_V956\JLink.exe',
    [string] $NmExe = 'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe',
    [string] $OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$fieldNames = @(
    'magic', 'version', 'command_generation', 'active_generation',
    'requested_line_events', 'running', 'start_line_event', 'end_line_event',
    'start_underflows', 'end_underflows', 'start_live_commits',
    'end_live_commits', 'start_spectrum_presents', 'end_spectrum_presents',
    'start_complete_windows', 'end_complete_windows', 'start_buffer_errors',
    'end_buffer_errors', 'start_overlay_presents', 'end_overlay_presents',
    'start_overlay_pixels', 'end_overlay_pixels', 'start_overlay_underflows',
    'end_overlay_underflows', 'start_overlay_fallbacks',
    'end_overlay_fallbacks', 'errors'
)
$expectedMagic = [uint32]0x4C495645

foreach($path in @($Elf, $JLinkExe, $NmExe)) {
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required live-monitor input was not found: $path"
    }
}

$Elf = (Resolve-Path -LiteralPath $Elf).Path
$JLinkExe = (Resolve-Path -LiteralPath $JLinkExe).Path
$NmExe = (Resolve-Path -LiteralPath $NmExe).Path
$ProbeSerial = $ProbeSerial.TrimStart('0')

$nmLine = @(& $NmExe -S -C $Elf) |
    Where-Object { $_ -match '\bg_rf_ui_runtime_monitor$' } |
    Select-Object -First 1
if(-not $nmLine -or
   $nmLine -notmatch '^(?<address>[0-9A-Fa-f]+)\s+(?<size>[0-9A-Fa-f]+)\s+\S\s+g_rf_ui_runtime_monitor$') {
    throw 'g_rf_ui_runtime_monitor was not found in the selected CPU1 ELF.'
}
$address = [Convert]::ToUInt32($Matches.address, 16)
$size = [Convert]::ToUInt32($Matches.size, 16)
if($size -ne ($fieldNames.Count * 4)) {
    throw "Unexpected runtime-monitor size: ELF=$size expected=$($fieldNames.Count * 4)."
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
        throw "J-Link live-monitor access failed.`n$text"
    }
    return ,$raw
}

function Read-MonitorSnapshot {
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
        throw 'J-Link did not return the complete runtime-monitor object.'
    }

    $snapshot = [ordered]@{}
    for($index = 0; $index -lt $fieldNames.Count; ++$index) {
        $snapshot[$fieldNames[$index]] = [uint32]$words[$index]
    }
    if($snapshot.magic -ne $expectedMagic -or $snapshot.version -ne 2U) {
        throw ('Unexpected runtime-monitor identity: magic=0x{0:X8}, version={1}' -f
               $snapshot.magic, $snapshot.version)
    }
    return $snapshot
}

$generation = [BitConverter]::ToUInt32([Guid]::NewGuid().ToByteArray(), 0)
if($generation -eq 0U) { $generation = 1U }
$requestedLineEvents = [uint32]($Seconds * $LineEventsPerSecond)
$requestedAddress = $address + (4U * 4U)
$generationAddress = $address + (2U * 4U)
[void](Invoke-JLinkCommands -Commands @(
    ('w4 0x{0:X8}, 0x{1:X8}' -f $requestedAddress, $requestedLineEvents),
    ('w4 0x{0:X8}, 0x{1:X8}' -f $generationAddress, $generation)
))

# The firmware owns the observation interval. The margin lets a 49.99 Hz
# panel complete the requested nominal 50 Hz line-event count before one final read.
Start-Sleep -Seconds ($Seconds + 4)
$snapshot = Read-MonitorSnapshot
if($snapshot.active_generation -ne $generation) {
    throw "Runtime monitor did not accept generation $generation."
}
if($snapshot.running -ne 0U) {
    throw "Runtime monitor is still running after the detached wait."
}

$underflowDelta = [uint32]($snapshot.end_underflows -
                            $snapshot.start_underflows)
$liveCommitDelta = [uint32]($snapshot.end_live_commits -
                             $snapshot.start_live_commits)
$spectrumPresentDelta = [uint32]($snapshot.end_spectrum_presents -
                                  $snapshot.start_spectrum_presents)
$completeWindowDelta = [uint32]($snapshot.end_complete_windows -
                                 $snapshot.start_complete_windows)
$bufferErrorDelta = [uint32]($snapshot.end_buffer_errors -
                              $snapshot.start_buffer_errors)
$lineEventDelta = [uint32]($snapshot.end_line_event -
                            $snapshot.start_line_event)
$overlayPresentDelta = [uint32]($snapshot.end_overlay_presents -
                                 $snapshot.start_overlay_presents)
$overlayPixelDelta = [uint32]($snapshot.end_overlay_pixels -
                               $snapshot.start_overlay_pixels)
$overlayUnderflowDelta = [uint32]($snapshot.end_overlay_underflows -
                                   $snapshot.start_overlay_underflows)
$overlayFallbackDelta = [uint32]($snapshot.end_overlay_fallbacks -
                                  $snapshot.start_overlay_fallbacks)
$success = $snapshot.errors -eq 0U -and
           $lineEventDelta -ge $requestedLineEvents -and
           $underflowDelta -eq 0U -and
           $bufferErrorDelta -eq 0U -and
           $overlayPresentDelta -gt 0U -and
           $overlayPixelDelta -gt 0U -and
           $overlayUnderflowDelta -eq 0U -and
           $overlayFallbackDelta -eq 0U -and
           $spectrumPresentDelta -gt 0U

$result = [ordered]@{
    CapturedAt = (Get-Date).ToString('o')
    ProbeSerial = $ProbeSerial
    Elf = $Elf
    ElfSha256 = (Get-FileHash -LiteralPath $Elf -Algorithm SHA256).Hash
    RequestedSeconds = $Seconds
    RequestedLineEvents = $requestedLineEvents
    ObservedLineEvents = $lineEventDelta
    UnderflowDelta = $underflowDelta
    LiveCommitDelta = $liveCommitDelta
    SpectrumPresentDelta = $spectrumPresentDelta
    CompleteWindowDelta = $completeWindowDelta
    BufferErrorDelta = $bufferErrorDelta
    OverlayPresentDelta = $overlayPresentDelta
    OverlayPixelDelta = $overlayPixelDelta
    OverlayUnderflowDelta = $overlayUnderflowDelta
    OverlayFallbackDelta = $overlayFallbackDelta
    FirmwareErrors = $snapshot.errors
    Success = $success
    Snapshot = $snapshot
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

[CmdletBinding()]
param(
    [string] $Elf = (Join-Path $PSScriptRoot '..\cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf'),
    [ValidatePattern('^[0-9]{6,20}$')] [string] $ProbeSerial = '1082495494',
    [string] $JLinkExe = 'C:\Program Files\SEGGER\JLink_V956\JLink.exe',
    [string] $NmExe = 'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe',
    [string] $OutputPath,
    [switch] $Halt
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach ($path in @($Elf, $JLinkExe, $NmExe))
{
    if (-not (Test-Path -LiteralPath $path -PathType Leaf))
    {
        throw "Required UI input diagnostic file was not found: $path"
    }
}

$Elf = (Resolve-Path -LiteralPath $Elf).Path
$JLinkExe = (Resolve-Path -LiteralPath $JLinkExe).Path
$NmExe = (Resolve-Path -LiteralPath $NmExe).Path
$ProbeSerial = $ProbeSerial.TrimStart('0')

$gt911Fields = @(
    'magic', 'initialized', 'i2c_address', 'init_error', 'last_error',
    'last_i2c_event', 'i2c_transfers', 'i2c_errors', 'resets', 'product_id',
    'firmware_version', 'config_x_max', 'config_y_max', 'polls',
    'ready_frames', 'touch_frames', 'last_status', 'last_touch_count',
    'last_track_id', 'last_x', 'last_y', 'last_size'
)
$lvglFields = @(
    'magic', 'version', 'poll_period_ms', 'owner_reads', 'poll_calls',
    'poll_errors', 'sample_updates', 'pressed_samples', 'press_transitions',
    'release_transitions', 'late_polls', 'last_state', 'last_x', 'last_y',
    'last_error', 'last_poll_tick_ms', 'max_poll_interval_ms',
    'last_press_tick_ms', 'last_release_tick_ms'
)
$rfUiFields = [Collections.Generic.List[string]]::new()
foreach ($name in @(
    'magic', 'version', 'events', 'handled_events', 'ignored_events',
    'last_control', 'last_value', 'last_event_code', 'last_event_tick_ms'
))
{
    [void] $rfUiFields.Add($name)
}
for ($index = 0; $index -lt 12; ++$index)
{
    [void] $rfUiFields.Add(('control_events[{0}]' -f $index))
}

$objects = [ordered]@{
    GT911 = [ordered]@{
        Symbol = 'g_gt911_diag'
        Fields = $gt911Fields
        Magic = [uint32] 0x47543931
        Version = $null
    }
    LVGL = [ordered]@{
        Symbol = 'g_lvgl_app_input_diag'
        Fields = $lvglFields
        Magic = [uint32] 0x544F5543
        Version = [uint32] 1
    }
    RFUI = [ordered]@{
        Symbol = 'g_rf_ui_input_diag'
        Fields = @($rfUiFields)
        Magic = [uint32] 0x55494E50
        Version = [uint32] 2
    }
}

$nmLines = @(& $NmExe -S -C $Elf)
foreach ($entry in $objects.GetEnumerator())
{
    $object = $entry.Value
    $symbol = [string] $object.Symbol
    $symbolPattern = [regex]::Escape($symbol)
    $line = $nmLines |
        Where-Object { $_ -match "\b$symbolPattern`$" } |
        Select-Object -First 1
    if (-not $line -or
        $line -notmatch "^(?<address>[0-9A-Fa-f]+)\s+(?<size>[0-9A-Fa-f]+)\s+\S\s+$symbolPattern`$")
    {
        throw "$symbol was not found in the selected CPU1 ELF."
    }

    $address = [Convert]::ToUInt32($Matches.address, 16)
    $size = [Convert]::ToUInt32($Matches.size, 16)
    $fields = @($object.Fields)
    if ($size -ne ($fields.Count * 4))
    {
        throw "$symbol layout mismatch: ELF=$size expected=$($fields.Count * 4)."
    }
    $object['Address'] = $address
    $object['Size'] = $size
    $object['Words'] = New-Object 'uint32[]' $fields.Count
    $object['Seen'] = New-Object 'bool[]' $fields.Count
}

$commands = [Collections.Generic.List[string]]::new()
if ($Halt)
{
    [void] $commands.Add('halt')
}
foreach ($object in $objects.Values)
{
    $addressHex = '0x{0:X8}' -f ([uint32] $object.Address)
    $countHex = '0x{0:X}' -f (@($object.Fields).Count)
    [void] $commands.Add("mem32 $addressHex, $countHex")
}
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
    throw "J-Link UI input diagnostic read failed.`n$rawText"
}

foreach ($line in $rawLines)
{
    $text = [string] $line
    if ($text -notmatch '^(?:J-Link>)?(?<address>[0-9A-Fa-f]{8})\s*=\s*(?<values>.*)$')
    {
        continue
    }
    $lineAddress = [Convert]::ToUInt32($Matches.address, 16)
    $tokens = @($Matches.values -split '\s+' |
        Where-Object { $_ -match '^[0-9A-Fa-f]{8}$' })
    foreach ($object in $objects.Values)
    {
        $address = [uint32] $object.Address
        $size = [uint32] $object.Size
        if (($lineAddress -lt $address) -or ($lineAddress -ge ($address + $size)))
        {
            continue
        }
        $start = [int](($lineAddress - $address) / 4)
        for ($offset = 0; $offset -lt $tokens.Count; ++$offset)
        {
            $wordIndex = $start + $offset
            if ($wordIndex -ge @($object.Fields).Count)
            {
                break
            }
            $object.Words[$wordIndex] =
                [Convert]::ToUInt32($tokens[$offset], 16)
            $object.Seen[$wordIndex] = $true
        }
        break
    }
}

$snapshots = [ordered]@{}
foreach ($entry in $objects.GetEnumerator())
{
    $object = $entry.Value
    $fields = @($object.Fields)
    if (@($object.Seen) -contains $false)
    {
        $missing = for ($index = 0; $index -lt $fields.Count; ++$index)
        {
            if (-not $object.Seen[$index]) { $fields[$index] }
        }
        throw "$($object.Symbol) was incomplete: $($missing -join ', ')."
    }

    $snapshot = [ordered]@{}
    for ($index = 0; $index -lt $fields.Count; ++$index)
    {
        $snapshot[$fields[$index]] = [uint32] $object.Words[$index]
    }
    if ($snapshot.magic -ne [uint32] $object.Magic)
    {
        throw ('Unexpected {0} magic: 0x{1:X8}' -f $object.Symbol,
               $snapshot.magic)
    }
    if (($null -ne $object.Version) -and
        ($snapshot.version -ne [uint32] $object.Version))
    {
        throw "Unexpected $($object.Symbol) version: $($snapshot.version)."
    }
    $snapshots[$entry.Key] = [ordered]@{
        Symbol = $object.Symbol
        Address = ('0x{0:X8}' -f ([uint32] $object.Address))
        Size = [uint32] $object.Size
        Snapshot = $snapshot
    }
}

$result = [ordered]@{
    CapturedAt = (Get-Date).ToString('o')
    ProbeSerial = $ProbeSerial
    Target = 'R7KA8P1KF_CPU0'
    CPU1Elf = $Elf
    CPU1ElfSha256 =
        (Get-FileHash -LiteralPath $Elf -Algorithm SHA256).Hash.ToUpperInvariant()
    AccessMode = if ($Halt) { 'short-halt' } else { 'live-memory' }
    Diagnostics = $snapshots
}
$json = $result | ConvertTo-Json -Depth 8
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

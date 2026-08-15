[CmdletBinding()]
param(
    [string] $Elf = (Join-Path $PSScriptRoot '..\cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf'),
    [string] $Header = (Join-Path $PSScriptRoot '..\cpu1\src\display_bringup.h'),
    [ValidatePattern('^[0-9]{6,20}$')] [string] $ProbeSerial = '1082495494',
    [ValidateRange(1, 100)] [int] $Resets = 20,
    [ValidateRange(1, 15)] [int] $DetachedStartupSeconds = 3,
    [ValidateRange(1, 30)] [int] $StableWindowSeconds = 2,
    [string] $ReadDiagScript = (Join-Path $PSScriptRoot 'read_display_diag.ps1'),
    [string] $ResetScript = (Join-Path $HOME '.codex\skills\ra8p1\scripts\ra8p1-debug-probe.ps1'),
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach ($path in @($Elf, $Header, $ReadDiagScript, $ResetScript))
{
    if (-not (Test-Path -LiteralPath $path -PathType Leaf))
    {
        throw "Required reset-soak input was not found: $path"
    }
}

$Elf = (Resolve-Path -LiteralPath $Elf).Path
$Header = (Resolve-Path -LiteralPath $Header).Path
$ReadDiagScript = (Resolve-Path -LiteralPath $ReadDiagScript).Path
$ResetScript = (Resolve-Path -LiteralPath $ResetScript).Path
$ProbeSerial = $ProbeSerial.TrimStart('0')
if (-not $OutputDirectory)
{
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $PSScriptRoot `
        "..\evidence\performance\V27_2\reset-flicker\reset-soak-$stamp"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

function Read-DisplayDiagnostic
{
    $json = @(& $ReadDiagScript -ProbeSerial $ProbeSerial -Elf $Elf `
        -Header $Header) -join "`n"
    return $json | ConvertFrom-Json
}

function Get-Delta
{
    param(
        [Parameter(Mandatory = $true)] $Before,
        [Parameter(Mandatory = $true)] $After,
        [Parameter(Mandatory = $true)] [string] $Field
    )
    return [int64]$After.$Field - [int64]$Before.$Field
}

$elfHash = (Get-FileHash -LiteralPath $Elf -Algorithm SHA256).Hash.ToUpperInvariant()
$cycles = [Collections.Generic.List[object]]::new()

for ($index = 1; $index -le $Resets; ++$index)
{
    Write-Progress -Activity 'Display startup reset soak' `
        -Status "reset $index / $Resets" `
        -PercentComplete (100.0 * ($index - 1) / $Resets)

    & $ResetScript -Action Reset -Core CPU0 -ProbeSerial $ProbeSerial |
        Out-Null

    # These waits are detached observation windows, not firmware delays.
    Start-Sleep -Seconds $DetachedStartupSeconds
    $startup = Read-DisplayDiagnostic
    Start-Sleep -Seconds $StableWindowSeconds
    $stable = Read-DisplayDiagnostic
    $a = $startup.Snapshot
    $b = $stable.Snapshot

    $stableFields = @(
        'glcdc_underflows',
        'overlay_underflows',
        'animation_buffer_errors',
        'overlay_errors',
        'video_status',
        'fatal_status',
        'phy_status'
    )
    $deltas = [ordered]@{}
    foreach ($field in $stableFields)
    {
        $deltas[$field] = Get-Delta $a $b $field
    }

    $startupReady =
        ([uint32]$a.stage -eq 6U) -and
        ([uint32]$a.running -eq 1U) -and
        ([uint32]$a.startup_pin_levels_valid -eq 3U) -and
        ([uint32]$a.startup_backlight_initial_level -eq 0U) -and
        ([uint32]$a.startup_reset_initial_level -eq 0U) -and
        ([uint32]$a.startup_backlight_low_asserted -eq 1U) -and
        ([uint32]$a.startup_reset_asserted -eq 1U) -and
        ([uint32]$a.startup_reset_released -eq 1U) -and
        ([uint32]$a.startup_warmstart_ioport_error -eq 0U) -and
        ([uint32]$a.startup_warmstart_backlight_cfg_error -eq 0U) -and
        ([uint32]$a.startup_warmstart_backlight_write_error -eq 0U) -and
        ([uint32]$a.startup_warmstart_reset_cfg_error -eq 0U) -and
        ([uint32]$a.startup_warmstart_reset_write_error -eq 0U) -and
        ([uint32]$a.startup_warmstart_reset_read_error -eq 0U) -and
        ([uint32]$a.startup_warmstart_reset_level -eq 0U) -and
        ([uint32]$a.startup_reset_low_hold_ms -eq 50U) -and
        ([uint32]$a.startup_reset_release_wait_ms -eq 120U) -and
        ([uint32]$a.startup_reset_assert_sequence -gt 0U) -and
        ([uint32]$a.startup_reset_assert_sequence -lt
         [uint32]$a.startup_reset_release_sequence) -and
        ([uint32]$a.startup_reset_release_sequence -lt
         [uint32]$a.startup_first_dsi_command_sequence) -and
        ([uint32]$a.startup_first_dsi_command_sequence -lt
         [uint32]$a.startup_backlight_enable_sequence) -and
        ([uint32]$a.startup_sequence_valid -eq 1U) -and
        ([uint32]$a.startup_black_framebuffer_ready -eq 1U) -and
        ([uint32]$a.startup_panel_configured -eq 1U) -and
        ([uint32]$a.startup_video_started -eq 1U) -and
        ([uint32]$a.overlay_enabled -eq 1U) -and
        ([uint32]$a.overlay_state -eq 5U) -and
        ([uint32]$a.startup_clean_vsync_required -eq 16U) -and
        ([uint32]$a.startup_clean_vsync_count -ge 16U) -and
        ([uint32]$a.startup_backlight_enable_attempts -eq 1U) -and
        ([uint32]$a.startup_backlight_enabled -eq 1U) -and
        ([uint32]$a.startup_backlight_readback -eq 1U) -and
        ([uint32]$a.startup_backlight_transitions -eq 1U) -and
        ([uint32]$a.startup_gate_last_error -eq 0U)

    $errorsZero =
        ([uint32]$a.animation_buffer_errors -eq 0U) -and
        ([uint32]$a.overlay_errors -eq 0U) -and
        ([uint32]$a.video_status -eq 0U) -and
        ([uint32]$a.fatal_status -eq 0U) -and
        ([uint32]$a.phy_status -eq 0U)

    $stableAfterBacklight =
        -not ($deltas.Values | Where-Object { $_ -ne 0 }) -and
        ([uint32]$a.startup_gate_steps -eq [uint32]$b.startup_gate_steps) -and
        ([uint32]$a.startup_before_underflows -eq
         [uint32]$b.startup_last_underflows) -and
        ([uint32]$a.startup_before_layer2_underflows -eq
         [uint32]$b.startup_last_layer2_underflows) -and
        ([uint32]$a.startup_before_buffer_errors -eq
         [uint32]$b.startup_last_buffer_errors) -and
        ([uint32]$a.startup_before_overlay_errors -eq
         [uint32]$b.startup_last_overlay_errors) -and
        ([uint32]$a.startup_before_video_status -eq
         [uint32]$b.startup_last_video_status) -and
        ([uint32]$a.startup_before_fatal_status -eq
         [uint32]$b.startup_last_fatal_status) -and
        ([uint32]$a.startup_before_phy_status -eq
         [uint32]$b.startup_last_phy_status)

    $cycle = [ordered]@{
        Reset = $index
        CapturedAt = (Get-Date).ToString('o')
        StartupReady = $startupReady
        ErrorsZero = $errorsZero
        StableAfterBacklight = $stableAfterBacklight
        Success = $startupReady -and $errorsZero -and $stableAfterBacklight
        StartupUnderflows = [uint32]$a.startup_before_underflows
        StartupLayer2Underflows = [uint32]$a.startup_before_layer2_underflows
        StableDeltas = $deltas
        CleanVSyncs = [uint32]$a.startup_clean_vsync_count
        CleanVSyncRestarts = [uint32]$a.startup_clean_vsync_restarts
        BacklightLineEvent = [uint32]$a.startup_backlight_line_event
        GateSteps = [uint32]$a.startup_gate_steps
        StartupSequence = [ordered]@{
            ResetAssert = [uint32]$a.startup_reset_assert_sequence
            ResetRelease = [uint32]$a.startup_reset_release_sequence
            FirstDsiCommand = [uint32]$a.startup_first_dsi_command_sequence
            FirstDsiCommandValue = [uint32]$a.startup_first_dsi_command
            BacklightEnable = [uint32]$a.startup_backlight_enable_sequence
            Valid = [uint32]$a.startup_sequence_valid
        }
        StartupSnapshot = $a
        StableSnapshot = $b
    }
    $cycles.Add([pscustomobject]$cycle)
    $cycle | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $OutputDirectory `
            ('reset-{0:D2}.json' -f $index)) -Encoding utf8
}

Write-Progress -Activity 'Display startup reset soak' -Completed
$passed = @($cycles | Where-Object Success).Count
$summary = [ordered]@{
    CapturedAt = (Get-Date).ToString('o')
    ProbeSerial = $ProbeSerial
    CPU1Elf = $Elf
    CPU1ElfSha256 = $elfHash
    RequestedResets = $Resets
    PassedResets = $passed
    FailedResets = $Resets - $passed
    DetachedStartupSeconds = $DetachedStartupSeconds
    StableWindowSeconds = $StableWindowSeconds
    AllPassed = $passed -eq $Resets
    Cycles = $cycles
}
$summaryJson = $summary | ConvertTo-Json -Depth 8
$summaryJson | Set-Content -LiteralPath (
    Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$summaryJson
if ($passed -ne $Resets) { exit 2 }

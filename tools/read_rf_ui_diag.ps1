[CmdletBinding()]
param(
    [string] $Elf = (Join-Path $PSScriptRoot '..\cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf'),
    [ValidatePattern('^[0-9]{6,20}$')] [string] $ProbeSerial = '1082495494',
    [string] $JLinkExe = 'C:\Program Files\SEGGER\JLink_V956\JLink.exe',
    [string] $NmExe = 'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe',
    [ValidateRange(0, 3600)] [int] $WindowSeconds = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

foreach($path in @($Elf, $JLinkExe, $NmExe)) {
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required diagnostic input was not found: $path"
    }
}

$fieldNames = @(
    'magic', 'version', 'state', 'request_generation', 'pending_channel',
    'committed_channel', 'build_channel', 'active_source', 'requests',
    'cancellations', 'complete_windows', 'stale_windows', 'build_starts',
    'build_restarts', 'build_chunks', 'build_rows', 'build_completions',
    'atomic_commits', 'last_chunk_bytes', 'max_chunk_bytes',
    'last_session_id', 'last_window_sequence', 'live_build_starts',
    'live_build_cancellations', 'live_build_chunks', 'live_build_rows',
    'live_build_completions', 'live_render_chunks', 'live_atomic_commits',
    'live_last_chunk_bytes', 'live_max_chunk_bytes', 'live_base_rebuilds',
    'live_incremental_builds', 'spectrum_presents',
    'waterfall_invalidations', 'waterfall_invalidated_rows',
    'spectrum_invalidations', 'spectrum_invalidated_rows',
    'waterfall_source_rebinds', 'spectrum_source_rebinds',
    'source_rebind_failures', 'last_waterfall_descriptor',
    'last_waterfall_data', 'last_waterfall_source',
    'last_waterfall_render_column', 'switch_metadata_stage_steps',
    'switch_metadata_stage_restarts', 'overlay_build_chunks',
    'overlay_build_rows', 'overlay_presents', 'overlay_pixels_advanced',
    'overlay_max_backlog_pixels', 'overlay_source_switches',
    'overlay_guard_bytes', 'overlay_guard_max_bytes',
    'overlay_box_refreshes', 'overlay_fallbacks',
    'overlay_last_fallback_error', 'overlay_frame_generation',
    'overlay_latched_generation', 'overlay_sync_starts',
    'overlay_sync_chunks', 'overlay_sync_rows',
    'overlay_sync_completions', 'overlay_sync_last_chunk_bytes',
    'overlay_sync_max_chunk_bytes', 'switch_catchup_passes',
    'switch_catchup_completions', 'switch_catchup_overwrite_restarts',
    'switch_catchup_head_mismatches', 'switch_catchup_backlog_at_render',
    'switch_catchup_max_backlog_at_render', 'live_catchup_passes',
    'live_catchup_completions', 'live_catchup_overwrite_cancellations',
    'live_catchup_head_mismatches', 'live_catchup_backlog_at_ready',
    'live_catchup_max_backlog_at_ready', 'switch_request_line_event',
    'switch_commit_line_event', 'switch_last_latency_line_events',
    'switch_max_latency_line_events', 'switch_metadata_refresh_deferrals',
    'switch_metadata_post_commit_refreshes',
    'overlay_latch_pven_deferrals',
    'overlay_latch_pven_wait_polls',
    'overlay_latch_pven_confirmations',
    'overlay_guard_clip_submits',
    'overlay_guard_clip_pixels',
    'overlay_guard_clip_zero_prefix_submits',
    'raw_boxes_received',
    'box_batches_waiting_for_fusion',
    'history_boxes_committed_working',
    'history_boxes_dropped_idle',
    'history_boxes_dropped_uncertain',
    'history_boxes_dropped_stale',
    'history_boxes_dropped_identity_mismatch',
    'history_boxes_dropped_out_of_history',
    'pending_box_batch_high_water',
    'last_committed_round_index',
    'switch_cache_hits', 'switch_cache_misses',
    'switch_cache_stale_misses', 'switch_cache_catchup_columns',
    'switch_cache_max_catchup_columns'
)

$nmLine = @(& $NmExe -S -C (Resolve-Path -LiteralPath $Elf).Path) |
    Where-Object { $_ -match '\bg_rf_ui_channel_switch_diag$' } |
    Select-Object -First 1
if(-not $nmLine -or
   $nmLine -notmatch '^(?<address>[0-9A-Fa-f]+)\s+(?<size>[0-9A-Fa-f]+)\s+\S\s+g_rf_ui_channel_switch_diag$') {
    throw 'g_rf_ui_channel_switch_diag was not found in the selected CPU1 ELF.'
}
$address = [Convert]::ToUInt32($Matches.address, 16)
$size = [Convert]::ToUInt32($Matches.size, 16)
if($size -ne ($fieldNames.Count * 4)) {
    throw "Unexpected rf_ui diagnostic size: ELF=$size expected=$($fieldNames.Count * 4)."
}

function Read-RfUiSnapshot {
    $addressHex = '0x{0:X8}' -f $address
    $countHex = '0x{0:X}' -f $fieldNames.Count
    $commands = @("mem32 $addressHex, $countHex", 'exit')
    $arguments = @(
        '-NoGui', '1', '-AutoConnect', '1', '-Device', 'R7KA8P1KF_CPU0',
        '-If', 'SWD', '-Speed', '4000', '-SelectEmuBySN', $ProbeSerial
    )
    $raw = @($commands | & $JLinkExe @arguments 2>&1)
    if(($LASTEXITCODE -ne 0) -or
       (($raw -join "`n") -notmatch "S/N:\s*$([regex]::Escape($ProbeSerial))") -or
       (($raw -join "`n") -notmatch 'Device "R7KA8P1KF_CPU0" selected') -or
       (($raw -join "`n") -match '(?i)cannot connect|connection failed|unknown command')) {
        throw "J-Link rf_ui diagnostic read failed.`n$($raw -join "`n")"
    }

    $words = New-Object 'uint32[]' $fieldNames.Count
    foreach($line in $raw) {
        $text = [string]$line
        if($text -notmatch '^(?:J-Link>)?(?<lineAddress>[0-9A-Fa-f]{8})\s*=\s*(?<values>.*)$') {
            continue
        }
        $lineAddress = [Convert]::ToUInt32($Matches.lineAddress, 16)
        $tokens = @($Matches.values -split '\s+' |
            Where-Object { $_ -match '^[0-9A-Fa-f]{8}$' })
        $start = [int](($lineAddress - $address) / 4)
        for($offset = 0; $offset -lt $tokens.Count; ++$offset) {
            $index = $start + $offset
            if($index -ge $words.Length) { break }
            if($index -ge 0) { $words[$index] = [Convert]::ToUInt32($tokens[$offset], 16) }
        }
    }

    $snapshot = [ordered]@{}
    for($index = 0; $index -lt $fieldNames.Count; ++$index) {
        $snapshot[$fieldNames[$index]] = [uint32]$words[$index]
    }
    if($snapshot.magic -ne 0x53574348) {
        throw ('Unexpected rf_ui diagnostic magic: 0x{0:X8}' -f $snapshot.magic)
    }
    if($snapshot.version -ne [uint32]16) {
        throw "Unexpected rf_ui diagnostic version: $($snapshot.version)"
    }
    return $snapshot
}

$first = Read-RfUiSnapshot
$second = $null
if($WindowSeconds -gt 0) {
    Start-Sleep -Seconds $WindowSeconds
    $second = Read-RfUiSnapshot
}

$result = [ordered]@{
    CapturedAt = (Get-Date).ToString('o')
    ProbeSerial = $ProbeSerial
    Elf = (Resolve-Path -LiteralPath $Elf).Path
    ElfSha256 = (Get-FileHash -LiteralPath $Elf -Algorithm SHA256).Hash
    Address = ('0x{0:X8}' -f $address)
    Snapshot = $first
}
if($null -ne $second) {
    $delta = [ordered]@{}
    foreach($name in @('requests','cancellations','build_starts','build_restarts',
                       'build_chunks','build_rows','build_completions','atomic_commits',
                       'live_build_starts','live_build_cancellations','live_build_chunks',
                       'live_build_rows','live_build_completions','live_render_chunks',
                       'live_atomic_commits','live_base_rebuilds',
                       'live_incremental_builds','spectrum_presents',
                       'waterfall_invalidations','waterfall_invalidated_rows',
                       'spectrum_invalidations','spectrum_invalidated_rows',
                       'waterfall_source_rebinds','spectrum_source_rebinds',
                       'source_rebind_failures','switch_metadata_stage_steps',
                       'switch_metadata_stage_restarts','overlay_build_chunks',
                       'overlay_build_rows','overlay_presents',
                       'overlay_pixels_advanced','overlay_source_switches',
                       'overlay_guard_bytes','overlay_box_refreshes',
                       'overlay_fallbacks','overlay_sync_starts',
                       'overlay_sync_chunks','overlay_sync_rows',
                       'overlay_sync_completions','switch_catchup_passes',
                       'switch_catchup_completions',
                       'switch_catchup_overwrite_restarts',
                       'switch_catchup_head_mismatches','live_catchup_passes',
                       'switch_cache_hits','switch_cache_misses',
                       'switch_cache_stale_misses',
                       'switch_cache_catchup_columns',
                       'live_catchup_completions',
                       'live_catchup_overwrite_cancellations',
                       'live_catchup_head_mismatches',
                       'switch_metadata_refresh_deferrals',
                       'switch_metadata_post_commit_refreshes',
                       'overlay_latch_pven_deferrals',
                       'overlay_latch_pven_wait_polls',
                       'overlay_latch_pven_confirmations',
                       'overlay_guard_clip_submits',
                       'overlay_guard_clip_pixels',
                       'overlay_guard_clip_zero_prefix_submits',
                       'raw_boxes_received',
                       'history_boxes_committed_working',
                       'history_boxes_dropped_idle',
                       'history_boxes_dropped_uncertain',
                       'history_boxes_dropped_stale',
                       'history_boxes_dropped_identity_mismatch',
                       'history_boxes_dropped_out_of_history')) {
        $delta[$name] = [uint32]($second[$name] - $first[$name])
    }
    $delta['underflow_note'] = 'Read GLCDC UF with read_display_diag.ps1; this structure has no UF field.'
    $result['SecondSnapshot'] = $second
    $result['WindowSeconds'] = $WindowSeconds
    $result['Delta'] = $delta
}

$result | ConvertTo-Json -Depth 5

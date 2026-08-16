[CmdletBinding()]
param(
    [string] $E2Root = 'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0',
    [switch] $Clean,
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$solution = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$layoutHelper = Join-Path $solution 'tools\project-layout.ps1'
if (-not (Test-Path -LiteralPath $layoutHelper -PathType Leaf))
{
    throw "Project layout helper not found: $layoutHelper"
}
. $layoutHelper
$layout = Resolve-Ra8p1ProjectLayout -Solution $solution
$cpu0 = $layout.Cpu0Directory
$cpu1 = $layout.Cpu1Directory
$shared = Join-Path $solution 'shared'
$cpu0Elf = $layout.Cpu0Elf
$cpu1Elf = $layout.Cpu1Elf
$cpu0BoardLinkerInfo = Join-Path $cpu0 'board\bsp_linker_info.h'
$cpu0BoardHeader = Join-Path $cpu0 'board\board.h'
$cpu0GeneratedLinkerInfo = Join-Path $cpu0 'Debug\bsp_linker_info.h'
$cpu0Map = $layout.Cpu0Map
$cpu1Map = $layout.Cpu1Map
$cpu1CampaignObject = Join-Path $cpu1 'Debug\src\framework\campaign_control.o'
$cpu1LvConf = Join-Path $cpu1 'src\lv_conf.h'
$sharedAbiGuard = Join-Path $solution 'tools\shared-abi-guard.ps1'
$crossCoreAbiVerifier = Join-Path $solution 'tools\verify-cross-core-abi.ps1'
$skillScripts = Join-Path $HOME '.codex\skills\ra8p1\scripts'
$sizeTool = Join-Path $E2Root 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-size.exe'
$nmTool = Join-Path $E2Root 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-nm.exe'
$objdumpTool = Join-Path $E2Root 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-objdump.exe'

foreach ($path in @($cpu0, $cpu1, $shared, $cpu0BoardHeader, $cpu1LvConf,
                    $sharedAbiGuard, $crossCoreAbiVerifier, $skillScripts,
                    $sizeTool, $nmTool, $objdumpTool))
{
    if (-not (Test-Path -LiteralPath $path))
    {
        throw "Required verification path not found: $path"
    }
}

. $sharedAbiGuard
$sharedBefore = Get-Ra8p1SharedAbiSnapshot -SharedDirectory $shared

$cpu0BoardText = Get-Content -LiteralPath $cpu0BoardHeader -Raw
if ($cpu0BoardText -notmatch '#define\s+RA_SRAM_END\s+\(0x220E2000UL\)')
{
    throw 'CPU0 RT-Thread heap end is not locked to the CPU0 Solution RAM boundary.'
}

$cpu1LvConfText = Get-Content -LiteralPath $cpu1LvConf -Raw
if (($cpu1LvConfText -notmatch '(?m)^\s*#define\s+LV_USE_DRAW_DAVE2D\s+1(?:\s|$)') -or
    ($cpu1LvConfText -notmatch '(?m)^\s*#define\s+LV_USE_RENESAS_GLCDC\s+0(?:\s|$)'))
{
    throw 'CPU1 project-owned LVGL configuration must enable D/AVE2D and keep the custom GLCDC flush path.'
}

if (-not $SkipBuild)
{
    $buildScript = Join-Path $solution 'build-solution.ps1'
    if ($Clean)
    {
        $buildOutput = @(& $buildScript -E2Root $E2Root -Clean)
    }
    else
    {
        $buildOutput = @(& $buildScript -E2Root $E2Root)
    }
    if (-not $?) { throw 'Dual-core build failed.' }

    $sharedAfterBuild = Get-Ra8p1SharedAbiSnapshot -SharedDirectory $shared
    Assert-Ra8p1SharedAbiUnchanged -Before $sharedBefore -After $sharedAfterBuild -Stage 'verify-solution build'
}

foreach ($elf in @($cpu0Elf, $cpu1Elf))
{
    if (-not (Test-Path -LiteralPath $elf))
    {
        throw "Expected ELF not found: $elf"
    }
}
foreach ($map in @($cpu0Map, $cpu1Map))
{
    if (-not (Test-Path -LiteralPath $map))
    {
        throw "Expected linker map not found: $map"
    }
}
if (-not (Test-Path -LiteralPath $cpu1CampaignObject))
{
    throw "Expected CPU1 campaign object not found: $cpu1CampaignObject"
}

$cpu0MapText = Get-Content -LiteralPath $cpu0Map -Raw
$cpu0RamEndMatch = [regex]::Match(
    $cpu0MapText,
    '(?m)^\s*0x([0-9a-fA-F]+)\s+__RAM_segment_used_end__\s*=')
if (-not $cpu0RamEndMatch.Success)
{
    throw 'CPU0 map is missing __RAM_segment_used_end__.'
}
$cpu0RamUsedEnd = [Convert]::ToUInt64($cpu0RamEndMatch.Groups[1].Value, 16)
if ($cpu0RamUsedEnd -ge [uint64]0x220E2000)
{
    throw ('CPU0 RAM reaches shared SRAM: 0x{0:X8}' -f $cpu0RamUsedEnd)
}

function Get-PartitionMacros([string] $Path)
{
    $macros = @{}
    foreach ($line in Get-Content -LiteralPath $Path)
    {
        if ($line -match '^#define\s+(BSP_PARTITION_[A-Z0-9_]+)\s+\(([^)]+)\)')
        {
            $macros[$matches[1]] = $matches[2]
        }
    }
    return $macros
}

$boardMacros = Get-PartitionMacros $cpu0BoardLinkerInfo
$generatedMacros = Get-PartitionMacros $cpu0GeneratedLinkerInfo
$partitionNames = @($boardMacros.Keys + $generatedMacros.Keys | Sort-Object -Unique)
foreach ($name in $partitionNames)
{
    if ($boardMacros[$name] -ne $generatedMacros[$name])
    {
        throw "CPU0 runtime partition macro differs from the Solution build: $name"
    }
}

& (Join-Path $skillScripts 'ra8p1-solution-inspect.ps1') -Solution $solution
if (-not $?) { throw 'Solution memory inspection failed.' }

& (Join-Path $skillScripts 'ra8p1-helium-check.ps1') -Elf $cpu0Elf -RequireMve
if (-not $?) { throw 'Helium/MVE inspection failed.' }

$cpu0Symbols = (& $nmTool --defined-only $cpu0Elf) -join "`n"
$cpu1Symbols = (& $nmTool --defined-only $cpu1Elf) -join "`n"
$cpu1CampaignObjectSymbols = (& $nmTool --defined-only $cpu1CampaignObject) -join "`n"
if ($cpu1CampaignObjectSymbols -notmatch '(?m)\bcpu1_campaign_owns_scheduler$')
{
    throw 'CPU1 campaign object is missing cpu1_campaign_owns_scheduler.'
}
$requiredCpu0 = @(
    'eth_iq_fast_consume',
    'rf_pipeline_start',
    'analysis_pipeline_init',
    'analysis_pipeline_ingest_s16',
    'analysis_pipeline_get_stats',
    'arm_cfft_q15',
    'npu_runner_infer_with_absolute',
    'npu_runner_absolute_dji_heatmap',
    'iq_npu_model_open',
    'iq_npu_model_invoke_with_absolute',
    'iq_npu_model_absolute_dji_heatmap',
    'rf_v27_absolute_aux_decode',
    'g_rf_v27_absolute_model',
    'ipc_bridge_cpu0_publish',
    'ipc_bridge_cpu0_display_publish',
    'g_eth_iq_fast_stats',
    'g_iq_ring',
    'g_npu_proof',
    'g_npu_benchmark',
    's_iq_npu_arena'
)
$requiredCpu1 = @(
    'display_bringup_run',
    'display_app_step',
    'display_app_request_capture',
    'ipc_bridge_cpu1_poll',
    'ipc_bridge_cpu1_display_poll',
    'ipc_bridge_cpu1_command_send',
    'cpu1_campaign_init',
    'cpu1_campaign_service',
    'cpu1_campaign_result_visible',
    'g_cpu1_campaign_control',
    'g_cpu1_campaign_proof',
    'rf_v27_activity_service_init',
    'rf_v27_activity_service_poll',
    'rf_v27_activity_service_take_round_decision',
    'rf_v27_activity_fusion_apply_round',
    'rf_v27_activity_fusion_get',
    'rf_ui_apply_fusion_round',
    'g_rf_v27_activity_config',
    'g_rf_v27_activity_proof',
    'lvgl_app_signal_update',
    'lvgl_app_frame_presented',
    'gt911_touch_init',
    'lv_draw_dave2d_init',
    'd2_opendevice',
    'd2_executerenderbuffer',
    'DRW_INT_IPL',
    'drw_int_isr',
    'lv_draw_sw_init',
    'g_ui_content_frame_count',
    'g_presented_frame_count',
    'g_lvgl_tick_ms'
)

foreach ($symbol in $requiredCpu0)
{
    if ($cpu0Symbols -notmatch "(?m)\b$([regex]::Escape($symbol))$")
    {
        throw "CPU0 ELF is missing required symbol: $symbol"
    }
}
foreach ($symbol in $requiredCpu1)
{
    if ($cpu1Symbols -notmatch "(?m)\b$([regex]::Escape($symbol))$")
    {
        throw "CPU1 ELF is missing required symbol: $symbol"
    }
}

$cpu0SizedSymbols = (& $nmTool -S --defined-only $cpu0Elf) -join "`n"
$npuArenaMatch = [regex]::Match(
    $cpu0SizedSymbols,
    '(?m)^\s*[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+[bB]\s+s_iq_npu_arena$')
if (-not $npuArenaMatch.Success)
{
    throw 'CPU0 ELF is missing the sized s_iq_npu_arena symbol.'
}
$npuArenaBytes = [Convert]::ToUInt64($npuArenaMatch.Groups[1].Value, 16)
if ($npuArenaBytes -ne [uint64]192176)
{
    throw ('CPU0 NPU arena size differs from the V27 contract: {0} bytes' -f
        $npuArenaBytes)
}

$cpu1Disassembly = (& $objdumpTool -d $cpu1Elf) -join "`n"
if ($cpu1Disassembly -notmatch '(?m)<lv_draw_sw_init>:')
{
    throw 'CPU1 ELF is missing the LVGL software-renderer fallback.'
}
if ($cpu1Disassembly -notmatch '(?m)<lv_draw_dave2d_init>:')
{
    throw 'CPU1 ELF is missing the LVGL D/AVE2D renderer.'
}

$cpu0Size = & $sizeTool --format=berkeley $cpu0Elf
$cpu1Size = & $sizeTool --format=berkeley $cpu1Elf

$abiOutput = @(& $crossCoreAbiVerifier -Cpu0Elf $cpu0Elf -Cpu1Elf $cpu1Elf -E2Root $E2Root)
if (-not $?) { throw 'CPU0/CPU1 DWARF ABI verification failed.' }

$sharedAfter = Get-Ra8p1SharedAbiSnapshot -SharedDirectory $shared
Assert-Ra8p1SharedAbiUnchanged -Before $sharedBefore -After $sharedAfter -Stage 'solution verification'

[pscustomobject]@{
    CPU0Elf = $cpu0Elf
    CPU0Timestamp = (Get-Item -LiteralPath $cpu0Elf).LastWriteTime
    CPU0Sha256 = (Get-FileHash -LiteralPath $cpu0Elf -Algorithm SHA256).Hash.ToUpperInvariant()
    CPU0Size = ($cpu0Size -join ' ')
    CPU1Elf = $cpu1Elf
    CPU1Map = $cpu1Map
    CPU1Timestamp = (Get-Item -LiteralPath $cpu1Elf).LastWriteTime
    CPU1Sha256 = (Get-FileHash -LiteralPath $cpu1Elf -Algorithm SHA256).Hash.ToUpperInvariant()
    CPU1Size = ($cpu1Size -join ' ')
    RequiredCPU0Symbols = $requiredCpu0.Count
    RequiredCPU1Symbols = $requiredCpu1.Count
    NpuArenaBytes = $npuArenaBytes
    VerifiedProjectRoots = @($cpu0, $cpu1)
    VerificationSourceScope = 'Current Solution CPU0/CPU1 only'
    RuntimePartitionMacrosVerified = $partitionNames.Count
    CPU0RamUsedEnd = ('0x{0:X8}' -f $cpu0RamUsedEnd)
    SharedAbiHeaders = $sharedAfter.HeaderCount
    SharedAbiSha256 = $sharedAfter.Digest
    CrossCoreAbiVerified = $true
    CPU1SoftwareRendererVerified = $true
    CPU1SoftwareFallbackVerified = $true
    CPU1Dave2DRendererVerified = $true
    CPU1Dave2DDriverSymbolsVerified = $true
    CPU1CampaignObjectApiVerified = $true
    SolutionMemoryVerified = $true
    MveVerified = $true
    Flashed = $false
}

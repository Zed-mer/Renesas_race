[CmdletBinding()]
param(
    [string] $Cpu0Elf,
    [string] $Cpu1Elf,
    [string] $E2Root = 'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0',
    [switch] $Json,
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$solution = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
$layoutHelper = Join-Path $PSScriptRoot 'project-layout.ps1'
if (-not (Test-Path -LiteralPath $layoutHelper -PathType Leaf))
{
    throw "Project layout helper not found: $layoutHelper"
}
. $layoutHelper
$layout = Resolve-Ra8p1ProjectLayout -Solution $solution
$expectedCpu0Elf = $layout.Cpu0Elf
$expectedCpu1Elf = $layout.Cpu1Elf
$displayStreamHeader = Join-Path $solution 'shared\display_stream.h'
if (-not (Test-Path -LiteralPath $displayStreamHeader -PathType Leaf))
{
    throw "Display stream header does not exist: $displayStreamHeader"
}

function Get-LiteralMacro
{
    param(
        [Parameter(Mandatory = $true)] [string] $Text,
        [Parameter(Mandatory = $true)] [string] $Name
    )

    $escaped = [regex]::Escape($Name)
    $match = [regex]::Match(
        $Text,
        "(?m)^\s*#define\s+$escaped\s+\(?(0x[0-9A-Fa-f]+|[0-9]+)(?:ULL|UL|LL|U|L)?\)?\s*(?:/\*.*\*/)?$")
    if (-not $match.Success)
    {
        throw "Literal macro $Name was not found in $displayStreamHeader."
    }
    $literal = $match.Groups[1].Value
    if ($literal.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase))
    {
        return [Convert]::ToUInt64($literal.Substring(2), 16)
    }
    return [Convert]::ToUInt64($literal, 10)
}

$displayStreamText = Get-Content -LiteralPath $displayStreamHeader -Raw
$expectedDisplayMacros = [ordered]@{
    RA8P1_DISPLAY_STREAM_VERSION = 4
    RA8P1_DISPLAY_PEAK_CHANNEL_COUNT = 2
    RA8P1_DISPLAY_SPECTRUM_CHANNEL_COUNT = 1
    RA8P1_DISPLAY_SPECTRUM_BINS = 256
}
foreach ($entry in $expectedDisplayMacros.GetEnumerator())
{
    $actual = Get-LiteralMacro -Text $displayStreamText -Name $entry.Key
    if ($actual -ne [uint64] $entry.Value)
    {
        throw "Display stream macro $($entry.Key) must be $($entry.Value), found $actual."
    }
}

function Resolve-LocalElf
{
    param(
        [string] $Candidate,
        [Parameter(Mandatory = $true)] [string] $Expected,
        [Parameter(Mandatory = $true)] [string] $CoreName
    )

    if ([string]::IsNullOrWhiteSpace($Candidate))
    {
        $Candidate = $Expected
    }

    $fullPath = [IO.Path]::GetFullPath($Candidate)
    $expectedPath = [IO.Path]::GetFullPath($Expected)
    if (-not $fullPath.Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase))
    {
        throw ("$CoreName ELF must be the Debug artifact inside this Solution. " +
               "Expected: $expectedPath; received: $fullPath")
    }
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf))
    {
        throw "$CoreName ELF does not exist: $fullPath"
    }

    return (Resolve-Path -LiteralPath $fullPath).Path
}

function Resolve-Gdb
{
    $candidates = @(
        (Join-Path $E2Root 'toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-gdb.exe'),
        'C:\Renesas\RA\e2studio_v2025-12_fsp_v6.4.0\toolchains\gcc_arm\13.2.rel1\bin\arm-none-eabi-gdb.exe'
    )
    foreach ($candidate in $candidates)
    {
        if (Test-Path -LiteralPath $candidate -PathType Leaf)
        {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw 'arm-none-eabi-gdb.exe was not found.'
}

function ConvertTo-NativeArgument
{
    param([AllowEmptyString()] [string] $Argument)

    if ($null -eq $Argument) { $Argument = '' }
    if (($Argument.Length -gt 0) -and ($Argument -notmatch '[\s"]'))
    {
        return $Argument
    }

    # Windows CreateProcess receives one command line.  Quote according to the
    # CommandLineToArgvW/CRT backslash-before-quote rules.
    $escaped = [regex]::Replace($Argument, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

function Invoke-NativeCaptured
{
    param(
        [Parameter(Mandatory = $true)] [string] $FilePath,
        [Parameter(Mandatory = $true)] [string[]] $ArgumentList
    )

    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = (($ArgumentList | ForEach-Object {
        ConvertTo-NativeArgument -Argument $_
    }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.CreateNoWindow = $true

    $process = New-Object Diagnostics.Process
    $process.StartInfo = $startInfo
    try
    {
        if (-not $process.Start())
        {
            throw "Failed to start native process: $FilePath"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $exitCode = $process.ExitCode
    }
    finally
    {
        $process.Dispose()
    }

    [pscustomobject]@{
        ExitCode      = $exitCode
        StandardOutput = $stdout
        StandardError  = $stderr
    }
}

function Get-TextSha256
{
    param([Parameter(Mandatory = $true)] [string] $Text)

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try
    {
        $bytes = $sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))
    }
    finally
    {
        $sha256.Dispose()
    }
    return ([BitConverter]::ToString($bytes)).Replace('-', '')
}

function Get-DwarfMemberLayout
{
    param(
        [Parameter(Mandatory = $true)] [string] $Layout,
        [Parameter(Mandatory = $true)] [string] $Field,
        [Parameter(Mandatory = $true)] [string] $Elf
    )

    $escaped = [regex]::Escape($Field)
    $match = [regex]::Match(
        $Layout,
        "(?m)^\s*/\*\s*(?<offset>\d+)\s*\|\s*(?<bytes>\d+)\s*\*/\s*[^;\r\n]*?\b$escaped(?<array>(?:\[[0-9]+\])*)\s*;")
    if (-not $match.Success)
    {
        throw "DWARF member $Field was not found in $Elf.`n$Layout"
    }
    return [pscustomobject]@{
        Offset = [int] $match.Groups['offset'].Value
        Bytes  = [int] $match.Groups['bytes'].Value
        Array  = $match.Groups['array'].Value
    }
}

function Assert-DisplayStreamContract
{
    param(
        [Parameter(Mandatory = $true)] $Types,
        [Parameter(Mandatory = $true)] [string] $Elf
    )

    $frameType = $Types['ra8p1_display_frame_t']
    $slotType = $Types['ra8p1_display_stream_slot_t']
    if ($frameType.Bytes -ne 504)
    {
        throw "Display frame must be 504 bytes in $Elf, found $($frameType.Bytes)."
    }
    if ($slotType.Bytes -ne 512)
    {
        throw "Display stream slot must be 512 bytes in $Elf, found $($slotType.Bytes)."
    }

    $frameMembers = @(
        [pscustomobject]@{ Name = 'magic';          Offset = 0;   Bytes = 4;   Array = '' },
        [pscustomobject]@{ Name = 'version';        Offset = 4;   Bytes = 2;   Array = '' },
        [pscustomobject]@{ Name = 'size';           Offset = 6;   Bytes = 2;   Array = '' },
        [pscustomobject]@{ Name = 'session_id';     Offset = 8;   Bytes = 4;   Array = '' },
        [pscustomobject]@{ Name = 'sequence';       Offset = 12;  Bytes = 4;   Array = '' },
        [pscustomobject]@{ Name = 'sample_rate_hz'; Offset = 16;  Bytes = 4;   Array = '' },
        [pscustomobject]@{ Name = 'fft_size';       Offset = 20;  Bytes = 4;   Array = '' },
        [pscustomobject]@{ Name = 'channel_mask';   Offset = 24;  Bytes = 4;   Array = '' },
        [pscustomobject]@{ Name = 'flags';          Offset = 28;  Bytes = 4;   Array = '' },
        [pscustomobject]@{ Name = 'peak_bin';       Offset = 32;  Bytes = 8;   Array = '[2]' },
        [pscustomobject]@{ Name = 'peak_power_q16'; Offset = 40;  Bytes = 8;   Array = '[2]' },
        [pscustomobject]@{ Name = 'spectrum';       Offset = 48;  Bytes = 256; Array = '[1][256]' },
        [pscustomobject]@{ Name = 'publish_tick';   Offset = 304; Bytes = 4;   Array = '' },
        [pscustomobject]@{ Name = 'analysis';       Offset = 308; Bytes = 196; Array = '' }
    )
    $actualFrameMembers = [ordered]@{}
    foreach ($expected in $frameMembers)
    {
        $actual = Get-DwarfMemberLayout -Layout $frameType.Layout -Field $expected.Name -Elf $Elf
        if (($actual.Offset -ne $expected.Offset) -or
            ($actual.Bytes -ne $expected.Bytes) -or
            ($actual.Array -ne $expected.Array))
        {
            throw (("Display frame member {0} mismatch in {1}: expected offset={2}, " +
                    "bytes={3}, array='{4}'; found offset={5}, bytes={6}, array='{7}'.") -f
                   $expected.Name, $Elf, $expected.Offset, $expected.Bytes, $expected.Array,
                   $actual.Offset, $actual.Bytes, $actual.Array)
        }
        $actualFrameMembers[$expected.Name] = $actual
    }

    $slotMembers = @(
        [pscustomobject]@{ Name = 'begin_sequence'; Offset = 0;   Bytes = 4;   Array = '' },
        [pscustomobject]@{ Name = 'payload';        Offset = 4;   Bytes = 504; Array = '' },
        [pscustomobject]@{ Name = 'end_sequence';   Offset = 508; Bytes = 4;   Array = '' }
    )
    foreach ($expected in $slotMembers)
    {
        $actual = Get-DwarfMemberLayout -Layout $slotType.Layout -Field $expected.Name -Elf $Elf
        if (($actual.Offset -ne $expected.Offset) -or
            ($actual.Bytes -ne $expected.Bytes) -or
            ($actual.Array -ne $expected.Array))
        {
            throw (("Display stream slot member {0} mismatch in {1}: expected offset={2}, " +
                    "bytes={3}; found offset={4}, bytes={5}.") -f
                   $expected.Name, $Elf, $expected.Offset, $expected.Bytes,
                   $actual.Offset, $actual.Bytes)
        }
    }

    return [ordered]@{
        Version = 4
        FrameBytes = $frameType.Bytes
        SlotBytes = $slotType.Bytes
        PeakChannels = 2
        SpectrumChannels = 1
        SpectrumBins = 256
        SpectrumOffset = $actualFrameMembers['spectrum'].Offset
        PublishTickOffset = $actualFrameMembers['publish_tick'].Offset
        AnalysisOffset = $actualFrameMembers['analysis'].Offset
    }
}

$contractTypes = @(
    'ra8p1_system_telemetry_t',
    'ra8p1_ui_command_t',
    'ra8p1_ipc_cpu0_state_t',
    'ra8p1_ipc_cpu1_state_t',
    'ra8p1_ipc_handshake_t',
    'ra8p1_telemetry_mailbox_t',
    'ra8p1_command_mailbox_t',
    'ra8p1_wifi_status_mailbox_t',
    'ra8p1_display_stream_control_t',
    'ra8p1_detection_box_t',
    'ra8p1_analysis_extension_t',
    'ra8p1_display_frame_t',
    'ra8p1_display_stream_slot_t',
    'ra8p1_display_tile_payload_t',
    'ra8p1_display_tile_slot_t',
    'ra8p1_latency_record_t'
)

function Read-TypeLayouts
{
    param(
        [Parameter(Mandatory = $true)] [string] $Gdb,
        [Parameter(Mandatory = $true)] [string] $Elf
    )

    $arguments = @('-batch', '-nx', '-ex', 'set pagination off')
    foreach ($typeName in $contractTypes)
    {
        $marker = "@@RA8P1_ABI_BEGIN:$typeName@@"
        $arguments += @('-ex', "echo $marker\n", '-ex', "ptype /o $typeName")
    }
    $arguments += $Elf

    $nativeResult = Invoke-NativeCaptured -FilePath $Gdb -ArgumentList $arguments
    if ($nativeResult.ExitCode -ne 0)
    {
        throw ("GDB type inspection failed for $Elf (exit $($nativeResult.ExitCode)).`n" +
               $nativeResult.StandardOutput + "`n" + $nativeResult.StandardError)
    }

    $knownEncodingWarning = $false
    $unexpectedDiagnostics = @()
    foreach ($diagnostic in @($nativeResult.StandardError -split "`r?`n"))
    {
        if ([string]::IsNullOrWhiteSpace($diagnostic)) { continue }
        if (($diagnostic -match '^warning: could not convert .+ from the host encoding \(CP1252\) to UTF-32\.$') -or
            ($diagnostic -eq 'This normally should not happen, please file a bug report.'))
        {
            $knownEncodingWarning = $true
            continue
        }
        $unexpectedDiagnostics += $diagnostic
    }
    if ($unexpectedDiagnostics.Count -ne 0)
    {
        throw ("GDB reported unexpected diagnostics for $Elf.`n" +
               ($unexpectedDiagnostics -join "`n"))
    }

    $text = [string] $nativeResult.StandardOutput
    $types = [ordered]@{}
    for ($index = 0; $index -lt $contractTypes.Count; $index++)
    {
        $typeName = $contractTypes[$index]
        $marker = "@@RA8P1_ABI_BEGIN:$typeName@@"
        $start = $text.IndexOf($marker, [StringComparison]::Ordinal)
        if ($start -lt 0)
        {
            throw "DWARF marker for $typeName was not returned for $Elf."
        }
        $start += $marker.Length

        if ($index + 1 -lt $contractTypes.Count)
        {
            $nextMarker = "@@RA8P1_ABI_BEGIN:$($contractTypes[$index + 1])@@"
            $end = $text.IndexOf($nextMarker, $start, [StringComparison]::Ordinal)
            if ($end -lt 0)
            {
                throw "DWARF marker following $typeName was not returned for $Elf."
            }
        }
        else
        {
            $end = $text.Length
        }

        $layoutLines = @($text.Substring($start, $end - $start) -split "`r?`n" |
            ForEach-Object { $_.TrimEnd() } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $layout = ($layoutLines -join "`n").Trim()
        $sizeMatch = [regex]::Match($layout, 'total size \(bytes\):\s*(\d+)')
        if (($layout -notmatch '(?m)^type = ') -or (-not $sizeMatch.Success))
        {
            throw "DWARF type $typeName was not found in $Elf.`n$layout"
        }

        $types[$typeName] = [ordered]@{
            Bytes        = [int] $sizeMatch.Groups[1].Value
            LayoutSha256 = Get-TextSha256 -Text $layout
            Layout       = $layout
        }
    }

    $mailboxLayout = [string] $types['ra8p1_command_mailbox_t'].Layout
    $endMatch = [regex]::Match(
        $mailboxLayout,
        '(?m)^\s*/\*\s*(\d+)\s*\|\s*4\s*\*/\s*volatile uint32_t end_sequence')
    if (-not $endMatch.Success)
    {
        throw "end_sequence offset was not found in $Elf.`n$mailboxLayout"
    }

    $displayStreamContract = Assert-DisplayStreamContract -Types $types -Elf $Elf

    $layoutManifest = (($contractTypes | ForEach-Object {
        '{0}|{1}|{2}' -f $_, $types[$_].Bytes, $types[$_].LayoutSha256
    }) -join "`n")

    $publicTypes = [ordered]@{}
    foreach ($typeName in $contractTypes)
    {
        $publicTypes[$typeName] = [ordered]@{
            Bytes        = $types[$typeName].Bytes
            LayoutSha256 = $types[$typeName].LayoutSha256
        }
    }

    return [ordered]@{
        Elf = (Resolve-Path -LiteralPath $Elf).Path
        ElfSha256 = (Get-FileHash -LiteralPath $Elf -Algorithm SHA256).Hash.ToUpperInvariant()
        GdbEncodingWarningIgnored = $knownEncodingWarning
        LayoutDigest = Get-TextSha256 -Text $layoutManifest
        Types = $publicTypes
        UiCommandBytes = $types['ra8p1_ui_command_t'].Bytes
        CommandMailboxBytes = $types['ra8p1_command_mailbox_t'].Bytes
        CommandMailboxEndSequenceOffset = [int] $endMatch.Groups[1].Value
        IpcHandshakeBytes = $types['ra8p1_ipc_handshake_t'].Bytes
        DisplayStream = $displayStreamContract
    }
}

if ($SelfTest)
{
    $fixtureTypes = [ordered]@{
        ra8p1_display_frame_t = [ordered]@{
            Bytes = 504
            Layout = @'
type = struct st_ra8p1_display_frame {
/*      0      |       4 */    uint32_t magic;
/*      4      |       2 */    uint16_t version;
/*      6      |       2 */    uint16_t size;
/*      8      |       4 */    uint32_t session_id;
/*     12      |       4 */    uint32_t sequence;
/*     16      |       4 */    uint32_t sample_rate_hz;
/*     20      |       4 */    uint32_t fft_size;
/*     24      |       4 */    uint32_t channel_mask;
/*     28      |       4 */    uint32_t flags;
/*     32      |       8 */    uint32_t peak_bin[2];
/*     40      |       8 */    uint32_t peak_power_q16[2];
/*     48      |     256 */    uint8_t spectrum[1][256];
/*    304      |       4 */    uint32_t publish_tick;
/*    308      |     196 */    ra8p1_analysis_extension_t analysis;
                               /* total size (bytes):  504 */
}
'@
        }
        ra8p1_display_stream_slot_t = [ordered]@{
            Bytes = 512
            Layout = @'
type = struct st_ra8p1_display_stream_slot {
/*      0      |       4 */    volatile uint32_t begin_sequence;
/*      4      |     504 */    ra8p1_display_frame_t payload;
/*    508      |       4 */    volatile uint32_t end_sequence;
                               /* total size (bytes):  512 */
}
'@
        }
    }
    $fixture = Assert-DisplayStreamContract -Types $fixtureTypes -Elf '<self-test-v4>'
    if (($fixture.SpectrumOffset -ne 48) -or
        ($fixture.PublishTickOffset -ne 304) -or
        ($fixture.AnalysisOffset -ne 308))
    {
        throw 'Display stream ABI self-test returned incorrect offsets.'
    }

    $fixtureTypes['ra8p1_display_frame_t'].Layout =
        $fixtureTypes['ra8p1_display_frame_t'].Layout.Replace(
            'uint8_t spectrum[1][256];', 'uint8_t spectrum[2][128];')
    $staleRejected = $false
    try
    {
        [void] (Assert-DisplayStreamContract -Types $fixtureTypes -Elf '<self-test-stale-v2>')
    }
    catch
    {
        $staleRejected = $true
    }
    if (-not $staleRejected)
    {
        throw 'Display stream ABI self-test accepted the stale [2][128] spectrum shape.'
    }
    Write-Output 'Self-test passed: v4 [1][256] layout accepted and stale v2 [2][128] rejected.'
    exit 0
}

$cpu0Path = Resolve-LocalElf -Candidate $Cpu0Elf -Expected $expectedCpu0Elf -CoreName 'CPU0'
$cpu1Path = Resolve-LocalElf -Candidate $Cpu1Elf -Expected $expectedCpu1Elf -CoreName 'CPU1'
$gdb = Resolve-Gdb
$layouts = @(
    (Read-TypeLayouts -Gdb $gdb -Elf $cpu0Path),
    (Read-TypeLayouts -Gdb $gdb -Elf $cpu1Path)
)
$first = $layouts[0]
$second = $layouts[1]

$mismatches = @()
foreach ($typeName in $contractTypes)
{
    $cpu0Type = $first.Types[$typeName]
    $cpu1Type = $second.Types[$typeName]
    if (($cpu0Type.Bytes -ne $cpu1Type.Bytes) -or
        ($cpu0Type.LayoutSha256 -ne $cpu1Type.LayoutSha256))
    {
        $mismatches += ('{0}: CPU0={1}B/{2}, CPU1={3}B/{4}' -f
            $typeName, $cpu0Type.Bytes, $cpu0Type.LayoutSha256,
            $cpu1Type.Bytes, $cpu1Type.LayoutSha256)
    }
}
if ($mismatches.Count -ne 0)
{
    throw (("CPU0/CPU1 shared DWARF ABI mismatch. Build both cores from the same " +
            "shared headers before flashing.`n{0}") -f ($mismatches -join "`n"))
}

$report = [ordered]@{
    EvidenceKind = 'ELF DWARF cross-core ABI inspection'
    Solution = $solution
    Gdb = $gdb
    ComparedTypes = $contractTypes.Count
    CPU0 = $first
    CPU1 = $second
    SharedAbiEqual = $true
    DisplayStreamContract = $first.DisplayStream
}
if ($Json)
{
    $report | ConvertTo-Json -Depth 8
}
else
{
    Write-Output (('Cross-core ABI OK: {0} complete DWARF layouts; command={1} bytes, ' +
                   'mailbox={2} bytes, end_sequence_offset={3}, handshake={4} bytes') -f
                  $contractTypes.Count, $first.UiCommandBytes, $first.CommandMailboxBytes,
                  $first.CommandMailboxEndSequenceOffset, $first.IpcHandshakeBytes)
    Write-Output (('Display stream v{0}: peak={1}, spectrum={2}x{3}, frame/slot={4}/{5} bytes, ' +
                   'spectrum/publish/analysis offsets={6}/{7}/{8}') -f
                  $first.DisplayStream.Version, $first.DisplayStream.PeakChannels,
                  $first.DisplayStream.SpectrumChannels, $first.DisplayStream.SpectrumBins,
                  $first.DisplayStream.FrameBytes, $first.DisplayStream.SlotBytes,
                  $first.DisplayStream.SpectrumOffset, $first.DisplayStream.PublishTickOffset,
                  $first.DisplayStream.AnalysisOffset)
    Write-Output ('DWARF layout SHA-256 CPU0: {0}' -f $first.LayoutDigest)
    Write-Output ('DWARF layout SHA-256 CPU1: {0}' -f $second.LayoutDigest)
    Write-Output ('CPU0 ELF SHA-256: {0}' -f $first.ElfSha256)
    Write-Output ('CPU1 ELF SHA-256: {0}' -f $second.ElfSha256)
    if ($first.GdbEncodingWarningIgnored -or $second.GdbEncodingWarningIgnored)
    {
        Write-Output 'Ignored known GDB CP1252-to-UTF-32 diagnostic after checking exit code and every requested layout.'
    }
}

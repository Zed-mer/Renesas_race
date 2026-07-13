[CmdletBinding()]
param(
    [int[]] $RateMbps = @(850, 880, 900, 910),
    [ValidateRange(1, 120)] [int] $DurationSeconds = 5,
    [string] $PortName = 'COM7',
    [ValidatePattern('^/tmp/[A-Za-z0-9._-]+$')] [string] $SenderPath = '/tmp/sdr_iq_udp',
    [string] $StatsAddress = '0x22000920',
    [string] $RmacDiagAddress = '0x220005F8'
)

$ErrorActionPreference = 'Stop'
$jlinkReader = Join-Path $PSScriptRoot 'ra8p1_jlink_run.ps1'

function Read-SerialFor {
    param([System.IO.Ports.SerialPort] $Port, [int] $Milliseconds)
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    $text = ''
    do {
        Start-Sleep -Milliseconds 50
        $text += $Port.ReadExisting()
    } while ([DateTime]::UtcNow -lt $deadline)
    return $text
}

function Read-SerialUntil {
    param(
        [System.IO.Ports.SerialPort] $Port,
        [string] $Marker,
        [int] $TimeoutSeconds
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $text = ''
    do {
        Start-Sleep -Milliseconds 50
        $text += $Port.ReadExisting()
        if ($text.Contains($Marker)) {
            return $text
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Timed out waiting for SDR marker $Marker. Output: $text"
}

function Convert-StatsHex {
    param([string] $Hex)
    $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        $bytes[$index] = [Convert]::ToByte($Hex.Substring($index * 2, 2), 16)
    }
    $words = New-Object uint32[] 16
    for ($index = 0; $index -lt $words.Length; $index++) {
        $words[$index] = [BitConverter]::ToUInt32($bytes, $index * 4)
    }
    return $words
}

function Convert-RmacDiagHex {
    param([string] $Hex)
    $bytes = New-Object byte[] ($Hex.Length / 2)
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        $bytes[$index] = [Convert]::ToByte($Hex.Substring($index * 2, 2), 16)
    }
    $words = New-Object uint32[] 25
    for ($index = 0; $index -lt $words.Length; $index++) {
        $words[$index] = [BitConverter]::ToUInt32($bytes, $index * 4)
    }
    return $words
}

$port = [System.IO.Ports.SerialPort]::new($PortName, 115200, 'None', 8, 'One')
$port.ReadTimeout = 500
$port.WriteTimeout = 1000
$results = @()

try {
    $port.Open()
    $port.DiscardInBuffer()
    $port.Write("`n")
    $prompt = Read-SerialFor -Port $port -Milliseconds 400
    if ($prompt -match 'login:') {
        $port.Write("root`n")
        $null = Read-SerialFor -Port $port -Milliseconds 400
        $port.Write("analog`n")
        $null = Read-SerialFor -Port $port -Milliseconds 800
    }
    $port.Write("stty -echo`n")
    $null = Read-SerialFor -Port $port -Milliseconds 300

    for ($run = 0; $run -lt $RateMbps.Count; $run++) {
        $rate = $RateMbps[$run]
        $marker = "__IQ_RUN_$($run)_DONE__"
        $port.DiscardInBuffer()
        $port.Write("$SenderPath synthetic $rate $DurationSeconds; echo $marker`n")
        $serialOutput = Read-SerialUntil -Port $port -Marker $marker -TimeoutSeconds ($DurationSeconds + 15)
        $senderMatch = [regex]::Match(
            $serialOutput,
            'SDR_IQ_UDP packets=(\d+) payload_bytes=(\d+) iq_bytes=(\d+) elapsed_us=(\d+) payload_mbps_x1000=(\d+) iq_mbps_x1000=(\d+) mode=(\w+) transport=(\w+) cpu=(\w+)'
        )
        if (-not $senderMatch.Success) {
            throw "Could not parse SDR result for run $run. Output: $serialOutput"
        }

        $readerOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $jlinkReader `
            -NoReset 1 -ReadAddressHex $statsAddress -ReadLength 64 -OutputHex 1)
        if ($LASTEXITCODE -ne 0) {
            throw "J-Link stats read failed for run $run"
        }
        $ramLine = $readerOutput | Where-Object { $_ -match '^RAM\[' } | Select-Object -Last 1
        $ramMatch = [regex]::Match($ramLine, '\]\s+([0-9a-fA-F]{128})$')
        if (-not $ramMatch.Success) {
            throw "Could not parse RA stats for run $run. Output: $($readerOutput -join ' | ')"
        }
        $stats = Convert-StatsHex -Hex $ramMatch.Groups[1].Value
        if (($stats[0] -ne 0x5149504B) -or ($stats[1] -ne 1)) {
            throw ('Unexpected RA stats header: magic=0x{0:X8} version={1}' -f $stats[0], $stats[1])
        }

        $diagReaderOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $jlinkReader `
            -NoReset 1 -ReadAddressHex $RmacDiagAddress -ReadLength 100 -OutputHex 1)
        if ($LASTEXITCODE -ne 0) {
            throw "J-Link RMAC diagnostic read failed for run $run"
        }
        $diagRamLine = $diagReaderOutput | Where-Object { $_ -match '^RAM\[' } | Select-Object -Last 1
        $diagRamMatch = [regex]::Match($diagRamLine, '\]\s+([0-9a-fA-F]{200})$')
        if (-not $diagRamMatch.Success) {
            throw "Could not parse RMAC diagnostics for run $run. Output: $($diagReaderOutput -join ' | ')"
        }
        $diag = Convert-RmacDiagHex -Hex $diagRamMatch.Groups[1].Value
        if (($diag[0] -ne 0x524D4143) -or ($diag[1] -ne 1)) {
            throw ('Unexpected RMAC diagnostic header: magic=0x{0:X8} version={1}' -f $diag[0], $diag[1])
        }

        $sentPackets = [uint64]$senderMatch.Groups[1].Value
        $receivedPackets = [uint64]$stats[3]
        $result = [pscustomobject]@{
            TargetMbps = $rate
            SentPackets = $sentPackets
            ReceivedPackets = $receivedPackets
            LostPackets = $sentPackets - $receivedPackets
            SequenceGaps = [uint64]$stats[5]
            Reordered = [uint64]$stats[6]
            Invalid = [uint64]$stats[7]
            SenderPayloadMbps = [uint64]$senderMatch.Groups[5].Value / 1000.0
            SenderIqMbps = [uint64]$senderMatch.Groups[6].Value / 1000.0
            ReceiverIqMbps = [uint64]$stats[11] / 1000.0
            Transport = $senderMatch.Groups[8].Value
            PauseTx = [uint64]$diag[7]
            PauseRx = [uint64]$diag[8]
            RxOverflow = [uint64]$diag[9]
            RmacRxErrors = [uint64]$diag[15]
            RmacRxFrames = [uint64]$diag[16]
            FcsErrors = [uint64]$diag[21]
            FragmentErrors = [uint64]$diag[22]
            MessageLostIrq = [uint64]$diag[23]
            GlobalErrorIrq = [uint64]$diag[24]
        }
        $results += $result
        $result | Format-List | Out-String | Write-Host
    }
}
finally {
    if ($port.IsOpen) {
        $port.Write("stty echo`n")
        Start-Sleep -Milliseconds 100
        $port.Close()
    }
    $port.Dispose()
}

$results | Format-Table -AutoSize
if ($results | Where-Object { ($_.LostPackets -ne 0) -or ($_.SequenceGaps -ne 0) -or ($_.Reordered -ne 0) -or ($_.Invalid -ne 0) }) {
    exit 2
}

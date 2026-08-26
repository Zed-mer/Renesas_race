[CmdletBinding()]
param(
    [Parameter(Position = 0)] [string] $LocalPath,
    [Parameter(Position = 1)] [string] $RemotePath,
    [string] $Port = 'COM3',
    [ValidateRange(1200, 3000000)] [int] $BaudRate = 115200,
    [ValidateRange(45, 4096)] [int] $ChunkBytes = 360,
    [ValidateRange(1, 20)] [int] $Retries = 5,
    [ValidateRange(100, 60000)] [int] $AckTimeoutMs = 5000,
    [ValidateRange(0, 1000)] [int] $LineDelayMs = 15,
    [ValidateSet('None', 'RtsCts', 'XOnXOff')] [string] $FlowControl = 'None',
    [ValidatePattern('^[0-7]{3,4}$')] [string] $Mode = '0755',
    [switch] $Json,
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:AnsiPattern = [regex]::new("`e\[[0-?]*[ -/]*[@-~]")
$script:EmptySha256 = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'

function Assert-RemotePath {
    param([Parameter(Mandatory)] [string] $Path)

    if ($Path -notmatch '^/tmp/[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
        throw "RemotePath must be one safe file directly below /tmp: $Path"
    }
    $leaf = [System.IO.Path]::GetFileName($Path)
    if ($leaf -in @('.', '..')) {
        throw "RemotePath may not name . or ..: $Path"
    }
}

function ConvertTo-UuCharacter {
    param([Parameter(Mandatory)] [int] $Value)

    $sixBits = $Value -band 0x3f
    if ($sixBits -eq 0) {
        return [char] 0x60
    }
    return [char] ($sixBits + 0x20)
}

function ConvertTo-UuLines {
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [byte[]] $Bytes,
        [Parameter(Mandatory)] [string] $OutputPath
    )

    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("begin 600 $OutputPath")
    for ($offset = 0; $offset -lt $Bytes.Length; $offset += 45) {
        $count = [Math]::Min(45, $Bytes.Length - $offset)
        $builder = [System.Text.StringBuilder]::new()
        [void] $builder.Append((ConvertTo-UuCharacter $count))
        for ($index = 0; $index -lt $count; $index += 3) {
            $a = [int] $Bytes[$offset + $index]
            $b = if (($index + 1) -lt $count) { [int] $Bytes[$offset + $index + 1] } else { 0 }
            $c = if (($index + 2) -lt $count) { [int] $Bytes[$offset + $index + 2] } else { 0 }
            [void] $builder.Append((ConvertTo-UuCharacter ($a -shr 2)))
            [void] $builder.Append((ConvertTo-UuCharacter ((($a -shl 4) -bor ($b -shr 4)) -band 0x3f)))
            [void] $builder.Append((ConvertTo-UuCharacter ((($b -shl 2) -bor ($c -shr 6)) -band 0x3f)))
            [void] $builder.Append((ConvertTo-UuCharacter ($c -band 0x3f)))
        }
        $lines.Add($builder.ToString())
    }
    $lines.Add('`')
    $lines.Add('end')
    return $lines.ToArray()
}

function ConvertFrom-UuLinesForTest {
    param([Parameter(Mandatory)] [string[]] $Lines)

    if (($Lines.Count -lt 3) -or ($Lines[0] -notmatch '^begin [0-7]{3,4} ')) {
        throw 'Invalid uuencode header.'
    }
    $result = [System.Collections.Generic.List[byte]]::new()
    for ($lineIndex = 1; $lineIndex -lt $Lines.Count; $lineIndex++) {
        $line = $Lines[$lineIndex]
        if ($line -eq 'end') {
            # Keep byte[0]/byte[1] as an array instead of letting the
            # PowerShell pipeline collapse it to $null or a scalar byte.
            return ,$result.ToArray()
        }
        if (($line -eq '`') -or ($line -eq ' ')) {
            continue
        }
        if ($line.Length -lt 1) {
            throw 'Empty uuencode data line.'
        }
        $count = (([int][char]$line[0]) - 0x20) -band 0x3f
        $decoded = [System.Collections.Generic.List[byte]]::new()
        for ($cursor = 1; ($cursor + 3) -lt $line.Length; $cursor += 4) {
            $v0 = (([int][char]$line[$cursor]) - 0x20) -band 0x3f
            $v1 = (([int][char]$line[$cursor + 1]) - 0x20) -band 0x3f
            $v2 = (([int][char]$line[$cursor + 2]) - 0x20) -band 0x3f
            $v3 = (([int][char]$line[$cursor + 3]) - 0x20) -band 0x3f
            $decoded.Add([byte](($v0 -shl 2) -bor ($v1 -shr 4)))
            $decoded.Add([byte]((($v1 -shl 4) -bor ($v2 -shr 2)) -band 0xff))
            $decoded.Add([byte]((($v2 -shl 6) -bor $v3) -band 0xff))
        }
        if ($decoded.Count -lt $count) {
            throw 'Truncated uuencode data line.'
        }
        for ($i = 0; $i -lt $count; $i++) {
            $result.Add($decoded[$i])
        }
    }
    throw 'Missing uuencode end marker.'
}

function Get-BytesSha256 {
    param([Parameter(Mandatory)] [AllowEmptyCollection()] [byte[]] $Bytes)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        # Windows PowerShell 5/.NET Framework has no Convert.ToHexString.
        return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function New-SerialTransport {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [int] $Rate,
        [Parameter(Mandatory)] [string] $Handshake,
        [Parameter(Mandatory)] [int] $WriteTimeoutMs
    )

    $serial = [System.IO.Ports.SerialPort]::new(
        $Name,
        $Rate,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One)
    $serial.Encoding = [System.Text.Encoding]::ASCII
    $serial.NewLine = "`n"
    $serial.ReadTimeout = 100
    $serial.WriteTimeout = $WriteTimeoutMs
    $serial.DtrEnable = $false
    $serial.RtsEnable = $false
    $serial.Handshake = switch ($Handshake) {
        'RtsCts' { [System.IO.Ports.Handshake]::RequestToSend }
        'XOnXOff' { [System.IO.Ports.Handshake]::XOnXOff }
        default { [System.IO.Ports.Handshake]::None }
    }
    $serial.Open()
    return @{
        Kind = 'Serial'
        Port = $serial
        Name = $Name
        LineDelayMs = $LineDelayMs
    }
}

function Close-Transport {
    param([Parameter(Mandatory)] [hashtable] $Transport)

    if (($Transport.Kind -eq 'Serial') -and $null -ne $Transport.Port) {
        if ($Transport.Port.IsOpen) {
            $Transport.Port.Close()
        }
        $Transport.Port.Dispose()
    }
    $Transport.Open = $false
}

function Add-FakeLine {
    param([hashtable] $Transport, [string] $Line)
    $Transport.Queue.Enqueue($Line)
}

function Invoke-FakePendingOperation {
    param([Parameter(Mandatory)] [hashtable] $Transport)

    $operation = $Transport.PendingOperation
    if ($null -eq $operation) {
        return
    }
    $Transport.PendingOperation = $null
    switch ($operation.Name) {
        'Probe' {
            if (-not $Transport.Files.ContainsKey($Transport.TargetPath)) {
                Add-FakeLine $Transport "__RA8UP_TARGET_$($operation.Nonce) M -1 -"
            }
            else {
                $bytes = [byte[]] $Transport.Files[$Transport.TargetPath]
                Add-FakeLine $Transport "__RA8UP_TARGET_$($operation.Nonce) F $($bytes.Length) $(Get-BytesSha256 $bytes)"
            }
        }
        'Initialize' {
            $Transport.Files[$Transport.PartPath] = [byte[]]::new(0)
            Add-FakeLine $Transport "__RA8UP_READY_$($operation.Nonce)"
        }
        'Block' {
            $block = $operation.Block
            if (-not $Transport.Files.ContainsKey($Transport.ChunkPath)) {
                Add-FakeLine $Transport "__RA8UP_NACK_$($operation.Nonce)_$($block.Index) missing"
                return
            }
            $chunk = [byte[]] $Transport.Files[$Transport.ChunkPath]
            if (($Transport.NackBlockOnce -eq $block.Index) -and (-not $Transport.NackUsed)) {
                $Transport.NackUsed = $true
                Add-FakeLine $Transport "__RA8UP_NACK_$($operation.Nonce)_$($block.Index) injected"
                return
            }
            if (($chunk.Length -ne $block.Length) -or ((Get-BytesSha256 $chunk) -ne $block.Sha256)) {
                Add-FakeLine $Transport "__RA8UP_NACK_$($operation.Nonce)_$($block.Index) chunk"
                return
            }
            $part = [byte[]] $Transport.Files[$Transport.PartPath]
            if ($part.Length -eq $block.Offset) {
                $joined = [byte[]]::new($part.Length + $chunk.Length)
                [Array]::Copy($part, 0, $joined, 0, $part.Length)
                [Array]::Copy($chunk, 0, $joined, $part.Length, $chunk.Length)
                $Transport.Files[$Transport.PartPath] = $joined
                $part = $joined
            }
            if ($part.Length -ne $block.NextOffset) {
                Add-FakeLine $Transport "__RA8UP_NACK_$($operation.Nonce)_$($block.Index) offset"
                return
            }
            if (($Transport.LoseAckBlockOnce -eq $block.Index) -and (-not $Transport.LostAckUsed)) {
                $Transport.LostAckUsed = $true
                return
            }
            Add-FakeLine $Transport "__RA8UP_ACK_$($operation.Nonce)_$($block.Index) $($block.NextOffset)"
        }
        'Verify' {
            $part = [byte[]] $Transport.Files[$Transport.PartPath]
            $hash = if ($Transport.FinalHashMismatch) { ('0' * 64) } else { Get-BytesSha256 $part }
            Add-FakeLine $Transport "__RA8UP_VERIFY_$($operation.Nonce) $($part.Length) $hash"
        }
        'Publish' {
            if ($Transport.RaceTargetBeforePublish) {
                $Transport.Files[$Transport.TargetPath] = [System.Text.Encoding]::ASCII.GetBytes('new-valid-owner')
            }
            $allowed = $false
            if ($operation.InitialKind -eq 'M') {
                $allowed = -not $Transport.Files.ContainsKey($Transport.TargetPath)
            }
            elseif ($operation.InitialKind -eq 'F') {
                $current = [byte[]] $Transport.Files[$Transport.TargetPath]
                $allowed = (($current.Length -eq 0) -and ((Get-BytesSha256 $current) -eq $script:EmptySha256))
            }
            if (-not $allowed) {
                Add-FakeLine $Transport "__RA8UP_PUBLISH_NACK_$($operation.Nonce) occupied"
                return
            }
            $part = [byte[]] $Transport.Files[$Transport.PartPath]
            $Transport.Files[$Transport.TargetPath] = $part
            [void] $Transport.Files.Remove($Transport.PartPath)
            Add-FakeLine $Transport "__RA8UP_PUBLISHED_$($operation.Nonce) $($part.Length) $(Get-BytesSha256 $part)"
        }
        'Cleanup' {
            [void] $Transport.Files.Remove($Transport.PartPath)
            [void] $Transport.Files.Remove($Transport.ChunkPath)
        }
        default { throw "Unknown fake operation $($operation.Name)." }
    }
}

function Write-TransportLine {
    param(
        [Parameter(Mandatory)] [hashtable] $Transport,
        [Parameter(Mandatory)] [AllowEmptyString()] [string] $Line
    )

    if ($Transport.Kind -eq 'Fake') {
        Add-FakeLine $Transport $Line
        if ($null -ne $Transport.HereDoc) {
            if ($Line -eq $Transport.HereDoc.Delimiter) {
                $decoded = ConvertFrom-UuLinesForTest $Transport.HereDoc.Lines.ToArray()
                $Transport.Files[$Transport.ChunkPath] = $decoded
                $Transport.HereDoc = $null
            }
            else {
                $Transport.HereDoc.Lines.Add($Line)
            }
            return
        }
        if ($Line -match "^rm -f '[^']+'; /usr/bin/uudecode -o '[^']+' <<'(?<delimiter>[^']+)'$") {
            $Transport.HereDoc = @{
                Delimiter = $Matches.delimiter
                Lines = [System.Collections.Generic.List[string]]::new()
            }
            return
        }
        if ($Line -match "printf '__RA8UP_SYNC_(?<nonce>[A-Fa-f0-9]+)\\n'") {
            Add-FakeLine $Transport "__RA8UP_SYNC_$($Matches.nonce)"
            return
        }
        Invoke-FakePendingOperation $Transport
        return
    }

    # The SDR console has ICRNL enabled. Sending CRLF creates two logical
    # newlines and inserts empty records into a uuencoded here-document.
    $payload = $Transport.Port.Encoding.GetBytes($Line + "`r")
    $Transport.Port.Write($payload, 0, $payload.Length)
    $deadline = [DateTime]::UtcNow.AddMilliseconds([Math]::Max(100, $Transport.Port.WriteTimeout))
    while (($Transport.Port.BytesToWrite -gt 0) -and ([DateTime]::UtcNow -lt $deadline)) {
        Start-Sleep -Milliseconds 1
    }
    if ($Transport.Port.BytesToWrite -gt 0) {
        throw "Serial transmit queue did not drain on $($Transport.Name)."
    }
    if ($Transport.LineDelayMs -gt 0) {
        Start-Sleep -Milliseconds $Transport.LineDelayMs
    }
}

function Send-TransportControlC {
    param([Parameter(Mandatory)] [hashtable] $Transport)

    if ($Transport.Kind -eq 'Fake') {
        $Transport.HereDoc = $null
        return
    }
    $payload = [byte[]] @(0x03, 0x0d)
    $Transport.Port.Write($payload, 0, $payload.Length)
    Start-Sleep -Milliseconds 50
}

function Read-TransportLine {
    param([Parameter(Mandatory)] [hashtable] $Transport)

    if ($Transport.Kind -eq 'Fake') {
        if ($Transport.Queue.Count -eq 0) {
            Start-Sleep -Milliseconds 1
            throw [System.TimeoutException]::new('Fake serial timeout.')
        }
        return [string] $Transport.Queue.Dequeue()
    }
    return $Transport.Port.ReadLine().TrimEnd("`r")
}

function Normalize-SerialLine {
    param([AllowEmptyString()] [string] $Line)

    $clean = $script:AnsiPattern.Replace($Line, '')
    $clean = $clean -replace '[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]', ''
    return $clean.Trim()
}

function Wait-ProtocolLine {
    param(
        [Parameter(Mandatory)] [hashtable] $Transport,
        [Parameter(Mandatory)] [regex] $Pattern,
        [Parameter(Mandatory)] [int] $TimeoutMs
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    do {
        try {
            $line = Normalize-SerialLine (Read-TransportLine $Transport)
            Write-Verbose "SDR: $line"
            $match = $Pattern.Match($line)
            if ($match.Success) {
                return $match
            }
        }
        catch [System.TimeoutException] {
            # Keep polling until the transaction deadline.
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw [System.TimeoutException]::new("Timed out waiting for SDR response $($Pattern.ToString()).")
}

function Sync-SdrShell {
    param(
        [Parameter(Mandatory)] [hashtable] $Transport,
        [Parameter(Mandatory)] [string] $Nonce,
        [Parameter(Mandatory)] [int] $TimeoutMs,
        [switch] $Interrupt
    )

    if ($Interrupt) {
        Send-TransportControlC $Transport
    }
    Write-TransportLine $Transport ''
    Write-TransportLine $Transport "printf '__RA8UP_SYNC_${Nonce}\n'"
    [void] (Wait-ProtocolLine $Transport ([regex]::new("^__RA8UP_SYNC_${Nonce}$")) $TimeoutMs)
}

function Set-PendingFakeOperation {
    param([hashtable] $Transport, [hashtable] $Operation)
    if ($Transport.Kind -eq 'Fake') {
        $Transport.PendingOperation = $Operation
    }
}

function Invoke-CommandForResponse {
    param(
        [Parameter(Mandatory)] [hashtable] $Transport,
        [Parameter(Mandatory)] [string] $Command,
        [Parameter(Mandatory)] [regex] $Pattern,
        [Parameter(Mandatory)] [int] $TimeoutMs,
        [hashtable] $FakeOperation
    )

    if ($null -ne $FakeOperation) {
        Set-PendingFakeOperation $Transport $FakeOperation
    }
    Write-TransportLine $Transport $Command
    return Wait-ProtocolLine $Transport $Pattern $TimeoutMs
}

function Get-RemoteTargetState {
    param(
        [hashtable] $Transport,
        [string] $TargetPath,
        [string] $Nonce,
        [int] $TimeoutMs
    )

    $command = "if [ -L '$TargetPath' ]; then k=L; z=-1; h=-; " +
               "elif [ -f '$TargetPath' ]; then k=F; z=`$(wc -c <'$TargetPath'); set -- `$(sha256sum '$TargetPath' 2>/dev/null); h=`$1; " +
               "elif [ -e '$TargetPath' ]; then k=O; z=-1; h=-; else k=M; z=-1; h=-; fi; " +
               "printf '__RA8UP_TARGET_${Nonce} %s %s %s\n' `"`$k`" `"`$z`" `"`$h`""
    $pattern = [regex]::new("^__RA8UP_TARGET_${Nonce} (?<kind>[MFLO]) (?<size>-?[0-9]+) (?<hash>[0-9a-f]{64}|-)$")
    $match = Invoke-CommandForResponse $Transport $command $pattern $TimeoutMs @{
        Name = 'Probe'; Nonce = $Nonce
    }
    return [pscustomobject]@{
        Kind = $match.Groups['kind'].Value
        Size = [int64] $match.Groups['size'].Value
        Sha256 = $match.Groups['hash'].Value
    }
}

function Initialize-RemoteUpload {
    param(
        [hashtable] $Transport,
        [string] $PartPath,
        [string] $ChunkPath,
        [string] $Nonce,
        [int] $TimeoutMs
    )

    $command = "if ! command -v uudecode >/dev/null 2>&1 || ! command -v sha256sum >/dev/null 2>&1; then " +
               "printf '__RA8UP_INIT_NACK_${Nonce} tools\n'; " +
               "elif [ -e '$PartPath' ] || [ -e '$ChunkPath' ]; then printf '__RA8UP_INIT_NACK_${Nonce} collision\n'; " +
               "else umask 077; : >'$PartPath' && printf '__RA8UP_READY_${Nonce}\n'; fi"
    $pattern = [regex]::new("^__RA8UP_(?<state>READY|INIT_NACK)_${Nonce}(?: (?<reason>.*))?$")
    $match = Invoke-CommandForResponse $Transport $command $pattern $TimeoutMs @{
        Name = 'Initialize'; Nonce = $Nonce
    }
    if ($match.Groups['state'].Value -ne 'READY') {
        throw "SDR rejected upload initialization: $($match.Groups['reason'].Value)"
    }
}

function Send-RemoteBlock {
    param(
        [hashtable] $Transport,
        [byte[]] $Bytes,
        [int] $Index,
        [int64] $Offset,
        [string] $PartPath,
        [string] $ChunkPath,
        [string] $Nonce,
        [int] $TimeoutMs
    )

    $nextOffset = $Offset + $Bytes.Length
    $chunkHash = Get-BytesSha256 $Bytes
    $delimiter = "__RA8UP_EOF_${Nonce}_${Index}__"
    Write-TransportLine $Transport "rm -f '$ChunkPath'; /usr/bin/uudecode -o '$ChunkPath' <<'$delimiter'"
    foreach ($line in (ConvertTo-UuLines $Bytes 'ra8up.chunk')) {
        Write-TransportLine $Transport $line
    }
    Write-TransportLine $Transport $delimiter

    $block = @{
        Index = $Index
        Offset = $Offset
        NextOffset = $nextOffset
        Length = $Bytes.Length
        Sha256 = $chunkHash
    }
    $command = "z=`$(wc -c <'$ChunkPath' 2>/dev/null); set -- `$(sha256sum '$ChunkPath' 2>/dev/null); h=`$1; p=`$(wc -c <'$PartPath' 2>/dev/null); " +
               "if [ `"`$z`" = '$($Bytes.Length)' ] && [ `"`$h`" = '$chunkHash' ]; then " +
               "if [ `"`$p`" = '$Offset' ]; then cat '$ChunkPath' >>'$PartPath'; p=`$(wc -c <'$PartPath'); fi; " +
               "if [ `"`$p`" = '$nextOffset' ]; then printf '__RA8UP_ACK_${Nonce}_${Index} %s\n' `"`$p`"; " +
               "else printf '__RA8UP_NACK_${Nonce}_${Index} offset\n'; fi; " +
               "else printf '__RA8UP_NACK_${Nonce}_${Index} chunk-%s-%s\n' `"`$z`" `"`$h`"; fi; rm -f '$ChunkPath'"
    $pattern = [regex]::new("^__RA8UP_(?<state>ACK|NACK)_${Nonce}_${Index}(?: (?<detail>[A-Za-z0-9._-]+))?$")
    Set-PendingFakeOperation $Transport @{ Name = 'Block'; Nonce = $Nonce; Block = $block }
    Write-TransportLine $Transport $command
    $match = Wait-ProtocolLine $Transport $pattern $TimeoutMs
    if ($match.Groups['state'].Value -ne 'ACK') {
        throw [System.IO.InvalidDataException]::new("SDR rejected block ${Index}: $($match.Groups['detail'].Value)")
    }
    if ([int64] $match.Groups['detail'].Value -ne $nextOffset) {
        throw [System.IO.InvalidDataException]::new("SDR acknowledged block $Index at the wrong offset.")
    }
}

function Verify-RemotePart {
    param(
        [hashtable] $Transport,
        [string] $PartPath,
        [string] $Nonce,
        [int64] $ExpectedSize,
        [string] $ExpectedHash,
        [int] $TimeoutMs
    )

    $command = "z=`$(wc -c <'$PartPath' 2>/dev/null); set -- `$(sha256sum '$PartPath' 2>/dev/null); h=`$1; " +
               "printf '__RA8UP_VERIFY_${Nonce} %s %s\n' `"`$z`" `"`$h`""
    $pattern = [regex]::new("^__RA8UP_VERIFY_${Nonce} (?<size>[0-9]+) (?<hash>[0-9a-f]{64})$")
    $match = Invoke-CommandForResponse $Transport $command $pattern $TimeoutMs @{
        Name = 'Verify'; Nonce = $Nonce
    }
    $actualSize = [int64] $match.Groups['size'].Value
    $actualHash = $match.Groups['hash'].Value
    if (($actualSize -ne $ExpectedSize) -or ($actualHash -ne $ExpectedHash)) {
        throw [System.IO.InvalidDataException]::new(
            "Remote temporary file failed final SHA-256 gate: size=$actualSize sha256=$actualHash")
    }
}

function Publish-RemotePart {
    param(
        [hashtable] $Transport,
        [string] $PartPath,
        [string] $TargetPath,
        [string] $Nonce,
        [pscustomobject] $InitialState,
        [int64] $ExpectedSize,
        [string] $ExpectedHash,
        [string] $FileMode,
        [int] $TimeoutMs
    )

    $guard = if ($InitialState.Kind -eq 'M') {
        "[ ! -e '$TargetPath' ] && [ ! -L '$TargetPath' ]"
    }
    else {
        "[ -f '$TargetPath' ] && [ ! -L '$TargetPath' ] && [ `"`$(wc -c <'$TargetPath')`" = '0' ] && " +
        "set -- `$(sha256sum '$TargetPath' 2>/dev/null); [ `"`$1`" = '$script:EmptySha256' ]"
    }
    $command = "if $guard; then chmod '$FileMode' '$PartPath' && mv -f '$PartPath' '$TargetPath'; " +
               "z=`$(wc -c <'$TargetPath' 2>/dev/null); set -- `$(sha256sum '$TargetPath' 2>/dev/null); h=`$1; " +
               "if [ `"`$z`" = '$ExpectedSize' ] && [ `"`$h`" = '$ExpectedHash' ]; then " +
               "printf '__RA8UP_PUBLISHED_${Nonce} %s %s\n' `"`$z`" `"`$h`"; " +
               "else printf '__RA8UP_PUBLISH_NACK_${Nonce} verify\n'; fi; " +
               "else printf '__RA8UP_PUBLISH_NACK_${Nonce} occupied\n'; fi"
    $pattern = [regex]::new("^__RA8UP_(?<state>PUBLISHED|PUBLISH_NACK)_${Nonce}(?: (?<a>[A-Za-z0-9._-]+)(?: (?<b>[A-Za-z0-9._-]+))?)?$")
    $match = Invoke-CommandForResponse $Transport $command $pattern $TimeoutMs @{
        Name = 'Publish'; Nonce = $Nonce; InitialKind = $InitialState.Kind
    }
    if ($match.Groups['state'].Value -ne 'PUBLISHED') {
        throw [System.IO.IOException]::new(
            "Atomic publish refused; destination was not overwritten: $($match.Groups['a'].Value)")
    }
    if (([int64] $match.Groups['a'].Value -ne $ExpectedSize) -or
        ($match.Groups['b'].Value -ne $ExpectedHash)) {
        throw [System.IO.InvalidDataException]::new('Published file did not pass the final remote verification.')
    }
}

function Remove-RemoteTemporaryFiles {
    param(
        [hashtable] $Transport,
        [string] $PartPath,
        [string] $ChunkPath
    )

    try {
        Set-PendingFakeOperation $Transport @{ Name = 'Cleanup' }
        Write-TransportLine $Transport "rm -f '$PartPath' '$ChunkPath'"
    }
    catch {
        Write-Verbose "Could not remove unique remote temporary files: $($_.Exception.Message)"
    }
}

function Invoke-SdrSerialUpload {
    param(
        [Parameter(Mandatory)] [byte[]] $Data,
        [Parameter(Mandatory)] [string] $Destination,
        [Parameter(Mandatory)] [hashtable] $Transport,
        [int] $BlockBytes = 360,
        [int] $MaximumRetries = 5,
        [int] $ResponseTimeoutMs = 5000,
        [string] $FileMode = '0755',
        [string] $UploadNonce
    )

    Assert-RemotePath $Destination
    if ($Data.Length -eq 0) {
        throw 'Refusing to upload an empty local file.'
    }
    if ([string]::IsNullOrWhiteSpace($UploadNonce)) {
        $UploadNonce = ([Guid]::NewGuid().ToString('N')).Substring(0, 16)
    }
    if ($UploadNonce -notmatch '^[0-9a-fA-F]{8,32}$') {
        throw 'Upload nonce must contain 8-32 hexadecimal characters.'
    }

    $localHash = Get-BytesSha256 $Data
    $leaf = [System.IO.Path]::GetFileName($Destination)
    $partPath = "/tmp/.ra8up.$UploadNonce.$leaf.part"
    $chunkPath = "/tmp/.ra8up.$UploadNonce.chunk"
    if ($Transport.Kind -eq 'Fake') {
        $Transport.TargetPath = $Destination
        $Transport.PartPath = $partPath
        $Transport.ChunkPath = $chunkPath
    }
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $initialized = $false
    $published = $false
    $retryCount = 0
    try {
        Sync-SdrShell $Transport $UploadNonce $ResponseTimeoutMs -Interrupt
        $initial = Get-RemoteTargetState $Transport $Destination $UploadNonce $ResponseTimeoutMs
        if (($initial.Kind -eq 'F') -and ($initial.Size -eq $Data.Length) -and ($initial.Sha256 -eq $localHash)) {
            return [pscustomobject]@{
                Status = 'already-present'
                RemotePath = $Destination
                Bytes = $Data.Length
                Sha256 = $localHash
                Blocks = 0
                Retries = 0
                ElapsedMs = $stopwatch.ElapsedMilliseconds
            }
        }
        if (($initial.Kind -eq 'L') -or ($initial.Kind -eq 'O')) {
            throw "Refusing to replace non-regular destination $Destination."
        }
        if (($initial.Kind -eq 'F') -and ($initial.Size -gt 0)) {
            throw "Refusing to overwrite an existing non-empty file at $Destination (sha256=$($initial.Sha256))."
        }
        if (($initial.Kind -eq 'F') -and (($initial.Size -ne 0) -or ($initial.Sha256 -ne $script:EmptySha256))) {
            throw "Refusing to replace destination with an invalid or unreadable state at $Destination."
        }

        Initialize-RemoteUpload $Transport $partPath $chunkPath $UploadNonce $ResponseTimeoutMs
        $initialized = $true
        $offset = 0
        $blockIndex = 0
        while ($offset -lt $Data.Length) {
            $count = [Math]::Min($BlockBytes, $Data.Length - $offset)
            $block = [byte[]]::new($count)
            [Array]::Copy($Data, $offset, $block, 0, $count)
            $sent = $false
            $lastError = $null
            for ($attempt = 1; $attempt -le $MaximumRetries; $attempt++) {
                try {
                    Send-RemoteBlock $Transport $block $blockIndex $offset $partPath $chunkPath $UploadNonce $ResponseTimeoutMs
                    $sent = $true
                    break
                }
                catch {
                    $lastError = $_.Exception
                    if ($attempt -lt $MaximumRetries) {
                        $retryCount++
                        Write-Verbose "Retrying block $blockIndex after attempt ${attempt}: $($lastError.Message)"
                        Sync-SdrShell $Transport $UploadNonce $ResponseTimeoutMs -Interrupt
                    }
                }
            }
            if (-not $sent) {
                throw "Block $blockIndex failed after $MaximumRetries attempts: $($lastError.Message)"
            }
            $offset += $count
            $blockIndex++
        }

        Verify-RemotePart $Transport $partPath $UploadNonce $Data.Length $localHash $ResponseTimeoutMs
        Publish-RemotePart $Transport $partPath $Destination $UploadNonce $initial $Data.Length $localHash $FileMode $ResponseTimeoutMs
        $published = $true
        return [pscustomobject]@{
            Status = 'uploaded'
            RemotePath = $Destination
            Bytes = $Data.Length
            Sha256 = $localHash
            Blocks = $blockIndex
            Retries = $retryCount
            ElapsedMs = $stopwatch.ElapsedMilliseconds
        }
    }
    finally {
        $stopwatch.Stop()
        if ($initialized -and (-not $published)) {
            Remove-RemoteTemporaryFiles $Transport $partPath $chunkPath
        }
    }
}

function New-FakeTransport {
    param(
        [hashtable] $InitialFiles = @{},
        [int] $LoseAckBlockOnce = -1,
        [int] $NackBlockOnce = -1,
        [switch] $FinalHashMismatch,
        [switch] $RaceTargetBeforePublish
    )

    $files = [System.Collections.Generic.Dictionary[string, byte[]]]::new()
    foreach ($entry in $InitialFiles.GetEnumerator()) {
        $files[[string]$entry.Key] = [byte[]] $entry.Value
    }
    return @{
        Kind = 'Fake'
        Open = $true
        Queue = [System.Collections.Generic.Queue[string]]::new()
        Files = $files
        HereDoc = $null
        PendingOperation = $null
        LoseAckBlockOnce = $LoseAckBlockOnce
        LostAckUsed = $false
        NackBlockOnce = $NackBlockOnce
        NackUsed = $false
        FinalHashMismatch = [bool] $FinalHashMismatch
        RaceTargetBeforePublish = [bool] $RaceTargetBeforePublish
        LineDelayMs = 0
    }
}

function Assert-TestCondition {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) {
        throw "Self-test failed: $Message"
    }
}

function Invoke-SelfTest {
    $random = [System.Random]::new(7020)
    foreach ($length in @(0, 1, 2, 3, 44, 45, 46, 359, 360, 361, 1024)) {
        $bytes = [byte[]]::new($length)
        $random.NextBytes($bytes)
        $decoded = ConvertFrom-UuLinesForTest (ConvertTo-UuLines $bytes '/tmp/chunk')
        Assert-TestCondition ($decoded.Length -eq $bytes.Length) "uuencode length $length"
        Assert-TestCondition ((Get-BytesSha256 $decoded) -eq (Get-BytesSha256 $bytes)) "uuencode bytes $length"
    }

    $payload = [byte[]]::new(1733)
    $random.NextBytes($payload)
    $fake = New-FakeTransport -LoseAckBlockOnce 0 -NackBlockOnce 2
    $result = Invoke-SdrSerialUpload $payload '/tmp/sdr_capture_agent' $fake 180 4 500 '0755' '1020304050607080'
    Assert-TestCondition ($result.Status -eq 'uploaded') 'fault-injected upload status'
    Assert-TestCondition ($result.Retries -eq 2) 'timeout and NACK retries counted'
    Assert-TestCondition ((Get-BytesSha256 ([byte[]]$fake.Files['/tmp/sdr_capture_agent'])) -eq (Get-BytesSha256 $payload)) 'retry is idempotent'

    $same = New-FakeTransport @{ '/tmp/sdr_capture_agent' = $payload }
    $sameResult = Invoke-SdrSerialUpload $payload '/tmp/sdr_capture_agent' $same 180 3 500 '0755' '1122334455667788'
    Assert-TestCondition ($sameResult.Status -eq 'already-present') 'same hash skips upload'

    $existing = [System.Text.Encoding]::ASCII.GetBytes('known-good-existing-binary')
    $occupied = New-FakeTransport @{ '/tmp/sdr_capture_agent' = $existing }
    $occupiedFailed = $false
    try {
        [void] (Invoke-SdrSerialUpload $payload '/tmp/sdr_capture_agent' $occupied 180 3 500 '0755' 'aabbccddeeff0011')
    }
    catch {
        $occupiedFailed = $true
    }
    Assert-TestCondition $occupiedFailed 'different non-empty destination refused'
    Assert-TestCondition ((Get-BytesSha256 ([byte[]]$occupied.Files['/tmp/sdr_capture_agent'])) -eq (Get-BytesSha256 $existing)) 'existing file preserved'

    $zero = [byte[]]::new(0)
    $badHash = New-FakeTransport @{ '/tmp/sdr_capture_agent' = $zero } -FinalHashMismatch
    $hashFailed = $false
    try {
        [void] (Invoke-SdrSerialUpload $payload '/tmp/sdr_capture_agent' $badHash 180 3 500 '0755' '1234567890abcdef')
    }
    catch {
        $hashFailed = $true
    }
    Assert-TestCondition $hashFailed 'final SHA-256 mismatch refused'
    Assert-TestCondition ($badHash.Files['/tmp/sdr_capture_agent'].Length -eq 0) 'final gate preserves old zero-byte target'

    $raced = New-FakeTransport @{} -RaceTargetBeforePublish
    $raceFailed = $false
    try {
        [void] (Invoke-SdrSerialUpload $payload '/tmp/sdr_capture_agent' $raced 180 3 500 '0755' 'fedcba0987654321')
    }
    catch {
        $raceFailed = $true
    }
    Assert-TestCondition $raceFailed 'publish race refused'
    Assert-TestCondition ([System.Text.Encoding]::ASCII.GetString($raced.Files['/tmp/sdr_capture_agent']) -eq 'new-valid-owner') 'racing destination preserved'

    $unsafeFailed = $false
    try { Assert-RemotePath '/tmp/../etc/passwd' } catch { $unsafeFailed = $true }
    Assert-TestCondition $unsafeFailed 'unsafe remote path refused'

    return [pscustomobject]@{
        Status = 'passed'
        UuencodeVectors = 11
        FaultInjectedRetries = $result.Retries
        ExistingFileGuard = $true
        FinalHashGate = $true
        PublishRaceGuard = $true
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($SelfTest) {
        Invoke-SelfTest | ConvertTo-Json -Depth 4
        exit 0
    }
    if ([string]::IsNullOrWhiteSpace($LocalPath) -or [string]::IsNullOrWhiteSpace($RemotePath)) {
        throw 'LocalPath and RemotePath are required unless -SelfTest is used.'
    }
    Assert-RemotePath $RemotePath
    $resolvedLocal = (Resolve-Path -LiteralPath $LocalPath -ErrorAction Stop).Path
    if (-not (Test-Path -LiteralPath $resolvedLocal -PathType Leaf)) {
        throw "Local file not found: $LocalPath"
    }
    $data = [System.IO.File]::ReadAllBytes($resolvedLocal)
    $transport = $null
    try {
        $transport = New-SerialTransport $Port $BaudRate $FlowControl ([Math]::Max(1000, $AckTimeoutMs))
        $result = Invoke-SdrSerialUpload $data $RemotePath $transport $ChunkBytes $Retries $AckTimeoutMs $Mode
        if ($Json) {
            $result | ConvertTo-Json -Depth 4
        }
        else {
            $result | Format-List
        }
    }
    finally {
        if ($null -ne $transport) {
            Close-Transport $transport
        }
    }
}

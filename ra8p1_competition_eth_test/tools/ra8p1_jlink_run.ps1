[CmdletBinding()]
param(
    [uint32] $SerialNumber = 1088229345,
    [string] $Device = 'R7KA8P1KF_CPU0',
    [ValidateRange(0, 120)] [int] $WaitSeconds = 0,
    [string] $ReadAddressHex = '',
    [ValidateRange(0, 4096)] [int] $ReadLength = 0,
    [ValidateSet(0, 1)] [int] $OutputHex = 0,
    [ValidateSet(0, 1)] [int] $NoReset = 0,
    [ValidateSet(0, 1)] [int] $HardReset = 0,
    [ValidateSet(0, 1)] [int] $ManualBoot = 0,
    [uint32] $StackPointer = 0x22020AA0,
    [uint32] $ProgramCounter = 0x0201EA74,
    [string] $DownloadElf = '',
    [ValidateSet(0, 1)] [int] $EraseChip = 0
)

$ErrorActionPreference = 'Stop'
if ([Environment]::Is64BitProcess) {
    $powershell32 = Join-Path $env:WINDIR 'SysWOW64\WindowsPowerShell\v1.0\powershell.exe'
    $forwardedArguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
        '-SerialNumber', $SerialNumber, '-Device', $Device, '-WaitSeconds', $WaitSeconds,
        '-ReadAddressHex', $ReadAddressHex, '-ReadLength', $ReadLength, '-OutputHex', $OutputHex,
        '-NoReset', $NoReset, '-HardReset', $HardReset, '-ManualBoot', $ManualBoot,
        '-StackPointer', $StackPointer, '-ProgramCounter', $ProgramCounter,
        '-EraseChip', $EraseChip
    )
    if ($DownloadElf) {
        $forwardedArguments += @('-DownloadElf', $DownloadElf)
    }
    & $powershell32 @forwardedArguments
    exit $LASTEXITCODE
}

$jlinkDll = Join-Path $env:USERPROFILE `
    '.eclipse\com.renesas.platform_583169726\DebugComp\RA\ARM\Segger_v9.14.1\JLinkARM.dll'
if (-not (Test-Path -LiteralPath $jlinkDll)) {
    throw "J-Link DLL not found: $jlinkDll"
}

if (-not ('Ra8p1JLink.Native' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

namespace Ra8p1JLink
{
    public static class Native
    {
        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINKARM_EMU_SelectByUSBSN(uint serialNumber);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr JLINKARM_Open();

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void JLINKARM_Close();

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int JLINKARM_ExecCommand(string command, byte[] response, int responseSize);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINKARM_TIF_Select(int targetInterface);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void JLINKARM_SetSpeed(int speedKhz);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINKARM_Connect();

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void JLINKARM_Halt();

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void JLINKARM_Reset();

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void JLINKARM_SetRESET();

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void JLINKARM_ClrRESET();

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINKARM_WriteU32(uint address, uint value);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINKARM_WriteReg(int registerIndex, uint value);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINKARM_WriteMem(uint address, uint count, byte[] data);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int JLINK_DownloadFile(string fileName, uint address);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINK_EraseChip();

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void JLINKARM_BeginDownload(uint flags);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINKARM_EndDownload();

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINKARM_GetRegisterList([Out] uint[] registerIndices, int capacity);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr JLINKARM_GetRegisterName(uint registerIndex);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern int JLINKARM_ReadMem(uint address, uint count, byte[] data);

        [DllImport(@"$jlinkDll", CallingConvention = CallingConvention.Cdecl)]
        public static extern void JLINKARM_Go();
    }
}
"@
}

$selected = [Ra8p1JLink.Native]::JLINKARM_EMU_SelectByUSBSN($SerialNumber)
if ($selected -lt 0) {
    throw "Failed to select J-Link S/N $SerialNumber (result $selected)"
}

$openError = [Ra8p1JLink.Native]::JLINKARM_Open()
if ($openError -ne [IntPtr]::Zero) {
    throw "J-Link open failed: $([Runtime.InteropServices.Marshal]::PtrToStringAnsi($openError))"
}

try {
    $response = New-Object byte[] 256
    $commandResult = [Ra8p1JLink.Native]::JLINKARM_ExecCommand("device=$Device", $response, $response.Length)
    if ($commandResult -lt 0) {
        throw "J-Link rejected device $Device (result $commandResult)"
    }
    if ([Ra8p1JLink.Native]::JLINKARM_TIF_Select(1) -lt 0) {
        throw 'Failed to select SWD'
    }
    [Ra8p1JLink.Native]::JLINKARM_SetSpeed(4000)
    if ([Ra8p1JLink.Native]::JLINKARM_Connect() -lt 0) {
        throw "Failed to connect to $Device"
    }

    [Ra8p1JLink.Native]::JLINKARM_Halt()
    if ($EraseChip) {
        $eraseResult = [Ra8p1JLink.Native]::JLINK_EraseChip()
        if ($eraseResult -lt 0) {
            throw "J-Link DLL chip erase failed with result $eraseResult"
        }
        Write-Output "J-Link DLL chip erase completed: result=$eraseResult"
    }
    if ($DownloadElf) {
        $elfPath = (Resolve-Path -LiteralPath $DownloadElf).Path
        $hexPath = [IO.Path]::ChangeExtension($elfPath, '.hex')
        if (-not (Test-Path -LiteralPath $hexPath)) {
            throw "Intel HEX not found beside ELF: $hexPath"
        }

        $segments = New-Object System.Collections.Generic.List[object]
        $segmentBytes = New-Object System.Collections.Generic.List[byte]
        [uint32]$segmentStart = 0
        [uint32]$upperAddress = 0
        foreach ($line in [IO.File]::ReadLines($hexPath)) {
            if (-not $line.StartsWith(':')) { throw "Invalid Intel HEX line: $line" }
            $text = $line.Substring(1)
            $record = New-Object byte[] ($text.Length / 2)
            [uint32]$sum = 0
            for ($i = 0; $i -lt $record.Length; $i++) {
                $record[$i] = [Convert]::ToByte($text.Substring($i * 2, 2), 16)
                $sum += $record[$i]
            }
            if (($sum -band 0xFF) -ne 0) { throw "Intel HEX checksum mismatch: $line" }

            $length = [uint32]$record[0]
            $offset = ([uint32]$record[1] -shl 8) -bor $record[2]
            $recordType = $record[3]
            if ($recordType -eq 0) {
                [uint32]$address = $upperAddress + $offset
                if ($segmentBytes.Count -eq 0) {
                    $segmentStart = $address
                }
                elseif ($address -ne ($segmentStart + $segmentBytes.Count)) {
                    $segments.Add([pscustomobject]@{ Address = $segmentStart; Data = $segmentBytes.ToArray() })
                    $segmentBytes = New-Object System.Collections.Generic.List[byte]
                    $segmentStart = $address
                }
                for ($i = 0; $i -lt $length; $i++) { $segmentBytes.Add($record[4 + $i]) }
            }
            elseif ($recordType -eq 4) {
                if ($segmentBytes.Count -gt 0) {
                    $segments.Add([pscustomobject]@{ Address = $segmentStart; Data = $segmentBytes.ToArray() })
                    $segmentBytes = New-Object System.Collections.Generic.List[byte]
                }
                $upperAddress = (([uint32]$record[4] -shl 8) -bor $record[5]) -shl 16
            }
            elseif ($recordType -eq 1) {
                break
            }
        }
        if ($segmentBytes.Count -gt 0) {
            $segments.Add([pscustomobject]@{ Address = $segmentStart; Data = $segmentBytes.ToArray() })
        }
        if (($segments.Count -eq 0) -or ($segments[0].Address -ne 0x02000000)) {
            throw 'Intel HEX does not begin at RA8P1 Code Flash address 0x02000000'
        }

        [Ra8p1JLink.Native]::JLINKARM_BeginDownload(0)
        [uint64]$downloadBytes = 0
        foreach ($segment in $segments) {
            $writeResult = [Ra8p1JLink.Native]::JLINKARM_WriteMem(
                [uint32]$segment.Address, [uint32]$segment.Data.Length, [byte[]]$segment.Data)
            if ($writeResult -lt 0) {
                throw "J-Link DLL write failed at 0x$($segment.Address.ToString('X8')) result=$writeResult"
            }
            $downloadBytes += $segment.Data.Length
        }
        $endResult = [Ra8p1JLink.Native]::JLINKARM_EndDownload()
        if ($endResult -lt 0) {
            throw "J-Link DLL download commit failed with result $endResult"
        }
        Write-Output "J-Link DLL HEX download completed: segments=$($segments.Count) bytes=$downloadBytes commit_result=$endResult file=$hexPath"
    }
    if ($HardReset) {
        [Ra8p1JLink.Native]::JLINKARM_SetRESET()
        Start-Sleep -Milliseconds 200
        [Ra8p1JLink.Native]::JLINKARM_ClrRESET()
        Start-Sleep -Milliseconds 500
        [Ra8p1JLink.Native]::JLINKARM_Halt()
    }
    elseif ($ManualBoot) {
        $vtorAddress = [Convert]::ToUInt32('E000ED08', 16)
        $registerList = New-Object uint32[] 256
        $registerCount = [Ra8p1JLink.Native]::JLINKARM_GetRegisterList($registerList, $registerList.Length)
        $registers = @{}
        for ($i = 0; $i -lt $registerCount; $i++) {
            $namePointer = [Ra8p1JLink.Native]::JLINKARM_GetRegisterName($registerList[$i])
            $name = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($namePointer)
            if ($name) { $registers[$name] = [int]$registerList[$i] }
        }
        $spIndex = @($registers.Keys | Where-Object { $_ -eq 'R13 (SP)' -or $_ -eq 'R13' -or $_ -eq 'SP' })[0]
        $pcIndex = @($registers.Keys | Where-Object { $_ -eq 'R15 (PC)' -or $_ -eq 'R15' -or $_ -eq 'PC' })[0]
        $xpsrIndex = @($registers.Keys | Where-Object { $_ -eq 'XPSR' })[0]
        if (-not $spIndex -or -not $pcIndex -or -not $xpsrIndex) {
            throw 'J-Link register enumeration did not return SP, PC, and XPSR'
        }
        if ([Ra8p1JLink.Native]::JLINKARM_WriteU32($vtorAddress, 0x02000000) -lt 0 -or
            [Ra8p1JLink.Native]::JLINKARM_WriteReg($registers[$spIndex], $StackPointer) -lt 0 -or
            [Ra8p1JLink.Native]::JLINKARM_WriteReg($registers[$pcIndex], $ProgramCounter) -lt 0 -or
            [Ra8p1JLink.Native]::JLINKARM_WriteReg($registers[$xpsrIndex], 0x01000000) -lt 0) {
            throw 'Failed to install manual reset context'
        }
        Write-Output "Manual boot registers: SP=$spIndex/$($registers[$spIndex]) PC=$pcIndex/$($registers[$pcIndex]) XPSR=$xpsrIndex/$($registers[$xpsrIndex])"
    }
    elseif (-not $NoReset) {
        [Ra8p1JLink.Native]::JLINKARM_Reset()
        [Ra8p1JLink.Native]::JLINKARM_Halt()
    }
    $fpbComparator0 = [Convert]::ToUInt32('E0002008', 16)
    if ([Ra8p1JLink.Native]::JLINKARM_WriteU32($fpbComparator0, 0) -lt 0) {
        throw 'Failed to clear FPB comparator 0'
    }
    [Ra8p1JLink.Native]::JLINKARM_Go()
    $runMode = if ($HardReset) { 'reset-pin pulse' } elseif ($ManualBoot) { 'manual vector boot' } elseif ($NoReset) { 'resume' } else { 'reset' }
    Write-Output "RA8P1 running: J-Link S/N $SerialNumber, mode=$runMode, FPB comparator 0 cleared"

    if (($ReadLength -gt 0) -and $ReadAddressHex) {
        Start-Sleep -Seconds $WaitSeconds
        $addressText = $ReadAddressHex -replace '^0[xX]', ''
        $address = [Convert]::ToUInt32($addressText, 16)
        $data = New-Object byte[] $ReadLength
        [Ra8p1JLink.Native]::JLINKARM_Halt()
        $read = [Ra8p1JLink.Native]::JLINKARM_ReadMem($address, $ReadLength, $data)
        [Ra8p1JLink.Native]::JLINKARM_Go()
        if ($read -lt 0) {
            throw "J-Link RAM read failed with result $read"
        }
        if ($OutputHex) {
            $text = [BitConverter]::ToString($data).Replace('-', '').ToLowerInvariant()
        }
        else {
            $text = [Text.Encoding]::ASCII.GetString($data).TrimEnd([char]0)
        }
        Write-Output "RAM[$ReadAddressHex,$ReadLength] $text"
    }
}
finally {
    [Ra8p1JLink.Native]::JLINKARM_Close()
}

<#
Shared ABI snapshot helpers.

Dot-source this file from build or verification scripts.  The snapshot covers
every header below the current Solution's shared directory so generated FSP
content cannot silently replace a cross-core contract between CPU0 and CPU1.
#>

function Get-Ra8p1SharedAbiSnapshot
{
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [string] $SharedDirectory
    )

    if (-not (Test-Path -LiteralPath $SharedDirectory -PathType Container))
    {
        throw "Shared ABI directory does not exist: $SharedDirectory"
    }

    $resolved = (Resolve-Path -LiteralPath $SharedDirectory).Path.TrimEnd(
        [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar))
    $headers = @(Get-ChildItem -LiteralPath $resolved -Filter '*.h' -File -Recurse |
        Sort-Object -Property FullName)
    if ($headers.Count -eq 0)
    {
        throw "Shared ABI directory contains no headers: $resolved"
    }

    $entries = @(
        foreach ($header in $headers)
        {
            $relative = $header.FullName.Substring($resolved.Length).TrimStart(
                [char[]]@([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)).Replace(
                    [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
            [pscustomobject]@{
                RelativePath = $relative
                Bytes        = [uint64] $header.Length
                Sha256       = (Get-FileHash -LiteralPath $header.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
            }
        }
    )

    $manifest = (($entries | ForEach-Object {
        '{0}|{1}|{2}' -f $_.RelativePath, $_.Bytes, $_.Sha256
    }) -join "`n")
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try
    {
        $digestBytes = $sha256.ComputeHash([Text.Encoding]::UTF8.GetBytes($manifest))
    }
    finally
    {
        $sha256.Dispose()
    }

    [pscustomobject]@{
        Directory   = $resolved
        HeaderCount = $entries.Count
        Digest      = ([BitConverter]::ToString($digestBytes)).Replace('-', '')
        Entries     = $entries
    }
}

function Assert-Ra8p1SharedAbiUnchanged
{
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] $Before,
        [Parameter(Mandatory = $true)] $After,
        [Parameter(Mandatory = $true)] [string] $Stage
    )

    $beforeByPath = @{}
    foreach ($entry in @($Before.Entries))
    {
        $beforeByPath[$entry.RelativePath] = $entry
    }

    $afterByPath = @{}
    foreach ($entry in @($After.Entries))
    {
        $afterByPath[$entry.RelativePath] = $entry
    }

    $changes = New-Object 'System.Collections.Generic.List[string]'
    foreach ($path in @($beforeByPath.Keys + $afterByPath.Keys | Sort-Object -Unique))
    {
        $beforeEntry = $beforeByPath[$path]
        $afterEntry = $afterByPath[$path]
        if ($null -eq $beforeEntry)
        {
            [void] $changes.Add("added: $path")
        }
        elseif ($null -eq $afterEntry)
        {
            [void] $changes.Add("removed: $path")
        }
        elseif (($beforeEntry.Sha256 -ne $afterEntry.Sha256) -or
                ($beforeEntry.Bytes -ne $afterEntry.Bytes))
        {
            [void] $changes.Add("changed: $path ($($beforeEntry.Sha256) -> $($afterEntry.Sha256))")
        }
    }

    if ($changes.Count -ne 0)
    {
        throw (("Shared ABI headers changed during {0}. Stop before flashing; " +
                "regenerate/recover both cores from the intended shared contract.`n{1}") -f
               $Stage, ($changes -join "`n"))
    }

    Write-Verbose ('Shared ABI snapshot unchanged after {0}: {1} headers, SHA-256 {2}' -f
                   $Stage, $After.HeaderCount, $After.Digest)
}

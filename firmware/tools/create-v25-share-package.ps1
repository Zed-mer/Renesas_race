[CmdletBinding()]
param(
    [string] $OutputDirectory,
    [string] $Label = '20260731',
    [string] $GitRef = 'HEAD'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $root 'delivery'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)

$packageName = "SUPER_V25_SHARE_$Label"
$stage = Join-Path $OutputDirectory $packageName
$archive = Join-Path $OutputDirectory ($packageName + '.zip')
$archiveHash = $archive + '.sha256'
foreach ($path in @($stage, $archive, $archiveHash)) {
    if (Test-Path -LiteralPath $path) {
        throw "Delivery path already exists: $path"
    }
}

New-Item -ItemType Directory -Path $stage -Force | Out-Null

function Copy-RequiredFile {
    param(
        [Parameter(Mandatory = $true)] [string] $Source,
        [Parameter(Mandatory = $true)] [string] $Destination
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required package input is missing: $Source"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Copy-OptionalFile {
    param(
        [Parameter(Mandatory = $true)] [string] $Source,
        [Parameter(Mandatory = $true)] [string] $Destination
    )
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
    }
}

function Copy-DirectoryFiles {
    param(
        [Parameter(Mandatory = $true)] [string] $Source,
        [Parameter(Mandatory = $true)] [string] $Destination
    )
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        return
    }
    $sourceRoot = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
    Get-ChildItem -LiteralPath $sourceRoot -File -Force -Recurse | ForEach-Object {
        $relative = $_.FullName.Substring($sourceRoot.Length).TrimStart('\')
        $target = Join-Path $Destination $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    }
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("super-v25-source-" + [guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
try {
    # Export only the committed source snapshot.  IDE state and build caches
    # from the caller's worktree are intentionally not part of the package.
    $sourceZip = Join-Path $tempRoot 'source.zip'
    & git -C $root archive --format=zip '--prefix=source/' "--output=$sourceZip" $GitRef
    if ($LASTEXITCODE -ne 0) {
        throw "git archive failed for ref $GitRef"
    }
    Expand-Archive -LiteralPath $sourceZip -DestinationPath $tempRoot -Force
    Copy-DirectoryFiles (Join-Path $tempRoot 'source') $stage
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

# Keep the repository README as a reference, and give the share package a
# focused entry point for the exact V25 binary/source pairing.
if (Test-Path -LiteralPath (Join-Path $stage 'README.md') -PathType Leaf) {
    Copy-Item -LiteralPath (Join-Path $stage 'README.md') -Destination (Join-Path $stage 'docs\README_repository_snapshot.md') -Force
}

Copy-RequiredFile (Join-Path $root 'cpu0\Debug\rtthread.elf') `
    (Join-Path $stage 'artifacts\ra8p1\cpu0\rtthread.elf')
Copy-RequiredFile (Join-Path $root 'cpu0\Debug\rtthread.map') `
    (Join-Path $stage 'artifacts\ra8p1\cpu0\rtthread.map')
Copy-OptionalFile (Join-Path $root 'cpu0\Debug\rtthread.hex') `
    (Join-Path $stage 'artifacts\ra8p1\cpu0\rtthread.hex')
Copy-RequiredFile (Join-Path $root 'cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf') `
    (Join-Path $stage 'artifacts\ra8p1\cpu1\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf')
Copy-RequiredFile (Join-Path $root 'cpu1\Debug\ra8p1_sdr_ai_display_solution_20260718_CPU1.map') `
    (Join-Path $stage 'artifacts\ra8p1\cpu1\ra8p1_sdr_ai_display_solution_20260718_CPU1.map')
Copy-RequiredFile (Join-Path $root 'build\ra8p1_sdr_ai_display_solution_20260718.sbd') `
    (Join-Path $stage 'build\ra8p1_sdr_ai_display_solution_20260718.sbd')

# Small, reviewable V25 verification evidence; do not copy the large build
# campaign directories.
Copy-OptionalFile (Join-Path $root 'build\v25-elf-verification-local.json') `
    (Join-Path $stage 'evidence\v25\v25-elf-verification-local.json')
Copy-OptionalFile (Join-Path $root 'build\v25-elf-verification.json') `
    (Join-Path $stage 'evidence\v25\v25-elf-verification.json')
Copy-DirectoryFiles (Join-Path $root 'artifacts\sdr\persistent_current') `
    (Join-Path $stage 'artifacts\sdr\persistent_current')
Copy-DirectoryFiles (Join-Path $root 'evidence\sdr_persistent_20260730') `
    (Join-Path $stage 'evidence\sdr_persistent_20260730')
Copy-DirectoryFiles (Join-Path $root 'evidence\tune_guard_e2e_20260729') `
    (Join-Path $stage 'evidence\tune_guard_e2e_20260729')

$cpu0Elf = Join-Path $stage 'artifacts\ra8p1\cpu0\rtthread.elf'
$cpu1Elf = Join-Path $stage 'artifacts\ra8p1\cpu1\ra8p1_sdr_ai_display_solution_20260718_CPU1.elf'
$cpu0Hash = (Get-FileHash -LiteralPath $cpu0Elf -Algorithm SHA256).Hash.ToUpperInvariant()
$cpu1Hash = (Get-FileHash -LiteralPath $cpu1Elf -Algorithm SHA256).Hash.ToUpperInvariant()
$head = (& git -C $root rev-parse "$GitRef^{commit}").Trim()
$branch = (& git -C $root branch --show-current).Trim()
$status = @(& git -C $root status --short)

$readme = @"
# SUPER V25 分享包

这是 V22 基线切换到 V25 四频活动融合后的源码、算法交接资料和当前双核构建制品。
源码来自 Git ref `$GitRef`（`$head`）；当前工作区的 e2 studio 本机元数据、Debug 中间文件和调试缓存没有纳入包内。

## 快速开始

```powershell
& .\tools\verify-share-package.ps1 -Root .
& .\build-solution.ps1
& .\verify-solution.ps1 -SkipBuild
& .\flash-solution.ps1 -ProbeSerial <实际 J-Link 序列号> -Run
```

CPU0 和 CPU1 必须通过 Solution 成套构建和烧录，不能单独下载 CPU1。
算法交接资料位于 `算法V25/`，其中包含 V25 压缩包、固件对接手册、回滚说明和参数核对表。

## 当前制品哈希

- CPU0 ELF: `$cpu0Hash`
- CPU1 ELF: `$cpu1Hash`

这两个 ELF 是本包打包时从 `cpu0/Debug` 和 `cpu1/Debug` 复制的快照；源代码改变后必须重新构建并重新计算哈希。

## 验证边界

本机已完成 CPU0/CPU1 官方构建、V25 ELF 静态核对和双核下载。包内 `evidence/v25/` 保存了小型核对结果；完整构建缓存、原始 IQ 和 IDE 工作区不在包内。
现场仍应执行连续四频切换和显示 UF 计数验收。
"@
Set-Content -LiteralPath (Join-Path $stage 'README.md') -Value $readme -Encoding utf8

$packageInfo = @(
    "Package: $packageName",
    "Created: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')",
    "Source Git ref: $GitRef",
    "Source Git commit: $head",
    "Source Git branch at packaging: $branch",
    "CPU0 ELF SHA-256: $cpu0Hash",
    "CPU1 ELF SHA-256: $cpu1Hash",
    'Scope: committed source snapshot + V25 algorithm handoff + current CPU0/CPU1 ELF/MAP + compact evidence',
    'Excluded: .git, IDE metadata, Debug/Release intermediates, build campaign caches, raw IQ and toolchains',
    'Worktree status is recorded in WORKTREE_STATUS.txt for provenance.'
)
Set-Content -LiteralPath (Join-Path $stage 'PACKAGE_INFO_V25.txt') -Value $packageInfo -Encoding utf8
Set-Content -LiteralPath (Join-Path $stage 'WORKTREE_STATUS.txt') -Value $status -Encoding utf8

$manifestPath = Join-Path $stage 'MANIFEST.sha256'
$manifestLines = Get-ChildItem -LiteralPath $stage -File -Force -Recurse |
    Where-Object { $_.FullName -ne $manifestPath } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($stage.Length).TrimStart('\').Replace('\', '/')
        '{0} *{1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToUpperInvariant(), $relative
    }
Set-Content -LiteralPath $manifestPath -Value $manifestLines -Encoding utf8

& (Join-Path $stage 'tools\verify-share-package.ps1') -Root $stage | Out-Null
Compress-Archive -LiteralPath $stage -DestinationPath $archive -CompressionLevel Optimal
$digest = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToUpperInvariant()
Set-Content -LiteralPath $archiveHash -Value ("$digest *$packageName.zip") -Encoding ascii

[pscustomobject]@{
    Status = 'created'
    Stage = $stage
    Archive = $archive
    ArchiveBytes = (Get-Item -LiteralPath $archive).Length
    ArchiveSha256 = $digest
    Manifest = $manifestPath
    ManifestFiles = $manifestLines.Count
}

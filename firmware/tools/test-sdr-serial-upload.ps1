[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$uploader = Join-Path $PSScriptRoot 'sdr-serial-upload.ps1'
$output = & $uploader -SelfTest
if ($LASTEXITCODE -ne 0) {
    throw "sdr-serial-upload.ps1 self-test failed with exit code $LASTEXITCODE."
}
$result = $output | ConvertFrom-Json
if ($result.Status -ne 'passed') {
    throw "Unexpected self-test status: $($result.Status)"
}
$result | ConvertTo-Json -Depth 4

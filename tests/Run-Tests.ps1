# 测试入口：依次执行 tests\*.Tests.ps1。
#
#   pwsh -File tests\Run-Tests.ps1 -BinDir build\debug
#   pwsh -File tests\Run-Tests.ps1 -BinDir build\debug -SkipServiceTests
#
# -SkipServiceTests 用于无法安装/启动服务的环境（沙箱、受限的 CI）。

[CmdletBinding()]
param(
    [string]$BinDir = (Join-Path $PSScriptRoot '..\build\debug'),
    [switch]$SkipServiceTests
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)

if (-not (Test-Path $BinDir)) {
    Write-Error "找不到二进制目录：$BinDir（先执行 cmake --build）"
    exit 2
}

$script:BinDir = (Resolve-Path $BinDir).Path
$script:SkipServiceTests = [bool]$SkipServiceTests

. (Join-Path $PSScriptRoot 'TestHelpers.ps1')

Write-Host "二进制目录：$script:BinDir"
if ($script:SkipServiceTests) {
    Write-Host '服务端集成测试：跳过' -ForegroundColor Yellow
}

Get-ChildItem -Path $PSScriptRoot -Filter '*.Tests.ps1' | Sort-Object Name | ForEach-Object {
    Write-Host ''
    Write-Host "== $($_.Name) ==" -ForegroundColor Cyan
    . $_.FullName
}

exit (Get-TestSummary)

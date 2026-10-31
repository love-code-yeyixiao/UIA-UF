# 极简测试框架：不依赖 Pester，PS 5.1 / pwsh 7 均可运行。

$script:TestResults = New-Object System.Collections.ArrayList

function Test {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        & $Body
        $stopwatch.Stop()
        [void]$script:TestResults.Add([pscustomobject]@{ Name = $Name; Passed = $true; Message = '' })
        Write-Host ("  PASS  {0} ({1} ms)" -f $Name, $stopwatch.ElapsedMilliseconds) -ForegroundColor Green
    }
    catch {
        $stopwatch.Stop()
        $message = $_.Exception.Message
        [void]$script:TestResults.Add([pscustomobject]@{ Name = $Name; Passed = $false; Message = $message })
        Write-Host ("  FAIL  {0}: {1}" -f $Name, $message) -ForegroundColor Red
    }
}

function Assert-True {
    param([bool]$Condition, [string]$Message = '条件不成立')
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Equal {
    param($Expected, $Actual, [string]$Message = '值不相等')
    if ($Expected -ne $Actual) {
        throw ("{0}（期望 '{1}'，实际 '{2}'）" -f $Message, $Expected, $Actual)
    }
}

function Assert-Match {
    param([string]$Text, [string]$Pattern, [string]$Message = '未匹配到预期内容')
    if ($Text -notmatch $Pattern) {
        throw ("{0}：/{1}/ 不在输出中「{2}」" -f $Message, $Pattern, $Text)
    }
}

function Invoke-Tool {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [string[]]$Arguments = @()
    )

    # 原生程序的 stderr 在 $ErrorActionPreference='Stop' 下会被当成终止错误，
    # 而 CLI 的用法说明正是走 stderr 的，所以这里临时放宽。
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $Exe @Arguments 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }

    [pscustomobject]@{
        Output   = $output
        ExitCode = $exitCode
    }
}

function Get-TestSummary {
    $failed = @($script:TestResults | Where-Object { -not $_.Passed })
    $passed = $script:TestResults.Count - $failed.Count

    Write-Host ''
    Write-Host ("合计 {0} 项，通过 {1}，失败 {2}" -f $script:TestResults.Count, $passed, $failed.Count)

    if ($failed.Count -eq 0) {
        return 0
    }

    Write-Host '失败项：' -ForegroundColor Red
    $failed | ForEach-Object { Write-Host ("  - {0}: {1}" -f $_.Name, $_.Message) -ForegroundColor Red }
    return 1
}

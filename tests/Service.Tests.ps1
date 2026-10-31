# 服务端集成测试：需要管理员权限并且能够安装/启动服务。
# 无法满足条件时整体跳过（CI 上应当正常执行）。

if ($script:SkipServiceTests) {
    Write-Host '  跳过：指定了 -SkipServiceTests' -ForegroundColor Yellow
    return
}

$service = Join-Path $script:BinDir 'UIA-UF.Service.exe'
$cli = Join-Path $script:BinDir 'uiauf.exe'

if (-not (Test-Path $service) -or -not (Test-Path $cli)) {
    Write-Host '  跳过：缺少 Service 或 CLI 二进制' -ForegroundColor Yellow
    return
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '  跳过：当前进程不在管理员组' -ForegroundColor Yellow
    return
}

$workDir = Join-Path $env:TEMP ('uiauf-tests-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $workDir -Force | Out-Null
$demo = Join-Path $workDir 'demo.exe'
# 用控制台程序而不是 notepad：CI runner 的作业进程在 session 0，没有可见桌面，
# GUI 程序会启动后立刻退出，check 就抓不到 PID。
Copy-Item (Join-Path $env:WINDIR 'System32\cmd.exe') $demo
$script:launchedPid = 0

try {
    Test '安装服务' {
        $result = Invoke-Tool $service @('--install')
        Assert-Equal 0 $result.ExitCode ("安装失败：" + $result.Output)
    }

    Test '服务在线' {
        $result = Invoke-Tool $cli @('ping')
        Assert-Equal 0 $result.ExitCode ("ping 失败：" + $result.Output)
        Assert-Match $result.Output 'PID' 'ping 输出异常'
    }

    Test '错误的预期哈希被拒绝' {
        $wrong = '0' * 64
        $result = Invoke-Tool $cli @('allow', 'add', $demo, '--sha256', $wrong)
        Assert-Equal 1 $result.ExitCode '错误哈希不应注册成功'
        Assert-Match $result.Output '不一致' '拒绝原因应为哈希不一致'
    }

    Test '注册白名单条目' {
        $hashResult = Invoke-Tool $cli @('hash', $demo)
        Assert-Equal 0 $hashResult.ExitCode ("计算哈希失败：" + $hashResult.Output)
        $digest = $hashResult.Output.Trim()

        $result = Invoke-Tool $cli @('allow', 'add', $demo, '--sha256', $digest)
        Assert-Equal 0 $result.ExitCode ("注册失败：" + $result.Output)
    }

    Test '列表显示条目与哈希' {
        $result = Invoke-Tool $cli @('allow', 'list')
        Assert-Equal 0 $result.ExitCode ("列表失败：" + $result.Output)
        Assert-Match $result.Output 'demo' '列表中没有 demo 条目'
        Assert-Match $result.Output '[0-9a-f]{64}' '列表中没有 64 位哈希'
    }

    Test '启动后令牌带有 UIAccess' {
        $result = Invoke-Tool $cli @('run', $demo, '/c', 'ping', '-n', '300', '127.0.0.1')
        Assert-Equal 0 $result.ExitCode ("启动失败：" + $result.Output)

        $match = [regex]::Match($result.Output, 'PID\s*=\s*(\d+)')
        Assert-True $match.Success ('输出中没有 PID：' + $result.Output)
        $script:launchedPid = [int]$match.Groups[1].Value

        $check = Invoke-Tool $cli @('check', "$script:launchedPid")
        Assert-Equal 0 $check.ExitCode ("check 失败：" + $check.Output)
        Assert-Match $check.Output 'UIAccess = yes' '目标令牌没有 UIAccess'
    }

    Test '未登记的程序被拒绝' {
        $result = Invoke-Tool $cli @('run', (Join-Path $env:WINDIR 'System32\notepad.exe'))
        Assert-Equal 1 $result.ExitCode '未登记的程序应被拒绝'
        Assert-Match $result.Output '不在白名单' '拒绝原因应为"不在白名单"，而不是其他失败'
    }

    Test '替换文件后哈希失配被拒绝' {
        if ($script:launchedPid -ne 0) {
            Stop-Process -Id $script:launchedPid -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 300
            $script:launchedPid = 0
        }

        # demo.exe 本身是 cmd.exe 的副本，必须写入不同内容才能制造哈希失配。
        [IO.File]::WriteAllBytes($demo, [byte[]](1..64))
        $result = Invoke-Tool $cli @('run', $demo)
        Assert-Equal 1 $result.ExitCode '文件被替换后仍被放行'
        Assert-Match $result.Output '哈希' '拒绝原因应为哈希不匹配'
    }

    Test '删除白名单条目' {
        $result = Invoke-Tool $cli @('allow', 'remove', 'demo')
        Assert-Equal 0 $result.ExitCode ("删除失败：" + $result.Output)
    }

    Test '删除后程序被拒绝' {
        $result = Invoke-Tool $cli @('run', $demo)
        Assert-Equal 1 $result.ExitCode '条目删除后仍被放行'
    }
}
finally {
    if ($script:launchedPid -ne 0) {
        Stop-Process -Id $script:launchedPid -Force -ErrorAction SilentlyContinue
    }
    Invoke-Tool $service @('--uninstall') | Out-Null
    Remove-Item -Recurse -Force $workDir -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------
# 闲置策略：到点自动禁用（只禁用 SCM 启动类型，不删除任何东西）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '  -- 闲置策略 --' -ForegroundColor Cyan

try {
    # 预置状态：白名单为空且 20 天没动过，并且已经提醒过 20 天。
    # 这样第二次评估必然进入"宽限期已过"的分支，不依赖是否有可交互会话。
    $allow = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\UIA-UF\Allow')
    $allow.SetValue('LastChanged', [UInt64]([DateTime]::UtcNow.AddDays(-20).ToFileTimeUtc()), 'QWord')
    $allow.Close()
    $setup = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\UIA-UF\Setup')
    $setup.SetValue('WarnedAt', [UInt64]([DateTime]::UtcNow.AddDays(-20).ToFileTimeUtc()), 'QWord')
    $setup.Close()

    Test '闲置宽限期结束后自动禁用服务' {
        $result = Invoke-Tool $service @('--install')
        Assert-Equal 0 $result.ExitCode ("安装失败：" + $result.Output)

        $disabled = $false
        for ($attempt = 0; $attempt -lt 50 -and -not $disabled; $attempt++) {
            Start-Sleep -Milliseconds 200
            $current = Get-Service -Name 'UIA-UF' -ErrorAction SilentlyContinue
            $disabled = ($null -ne $current) -and ($current.StartType -eq 'Disabled')
        }
        Assert-True $disabled '服务没有被自动禁用'

        $marker = Get-ItemProperty 'HKLM:\SOFTWARE\UIA-UF\Setup' -Name 'AutoDisabledAt' -ErrorAction SilentlyContinue
        Assert-True ($null -ne $marker) '缺少 AutoDisabledAt 标记'
    }

    Test '禁用后 CLI 提示已自动禁用' {
        $result = Invoke-Tool $cli @('ping')
        Assert-Equal 3 $result.ExitCode '服务被禁用时 ping 应返回 3'
        Assert-Match $result.Output '禁用' '提示中没有说明服务已被禁用'
    }

    Test '重新安装可恢复启用' {
        $result = Invoke-Tool $service @('--install')
        Assert-Equal 0 $result.ExitCode ("恢复失败：" + $result.Output)

        $ping = Invoke-Tool $cli @('ping')
        Assert-Equal 0 $ping.ExitCode ("恢复后服务未运行：" + $ping.Output)
    }
}
finally {
    Invoke-Tool $service @('--uninstall') | Out-Null
    Remove-Item 'HKLM:\SOFTWARE\UIA-UF' -Recurse -Force -ErrorAction SilentlyContinue
}

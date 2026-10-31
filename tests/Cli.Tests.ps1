# CLI 行为测试：不依赖服务，任何环境都能跑。

$cli = Join-Path $script:BinDir 'uiauf.exe'

Test '构建产物存在' {
    Assert-True (Test-Path $cli) "缺少 $cli"
}

Test 'help 退出码为 0' {
    $result = Invoke-Tool $cli @('help')
    Assert-Equal 0 $result.ExitCode ("help 失败：" + $result.Output)
    Assert-Match $result.Output 'uiauf' 'help 输出异常'
}

Test '未知命令退出码为 2' {
    $result = Invoke-Tool $cli @('definitely-not-a-command')
    Assert-Equal 2 $result.ExitCode '未知命令应返回 2'
}

Test 'run 缺少参数返回 2' {
    $result = Invoke-Tool $cli @('run')
    Assert-Equal 2 $result.ExitCode 'run 缺少参数应返回 2'
}

Test 'run 目标不存在返回 2' {
    $result = Invoke-Tool $cli @('run', 'C:\definitely\missing.exe')
    Assert-Equal 2 $result.ExitCode '目标不存在应返回 2'
}

Test 'check 非法 PID 返回 2' {
    $result = Invoke-Tool $cli @('check', 'not-a-pid')
    Assert-Equal 2 $result.ExitCode '非法 PID 应返回 2'
}

Test 'notify 缺少 message 返回 2' {
    $result = Invoke-Tool $cli @('notify', '--title', 'x')
    Assert-Equal 2 $result.ExitCode 'notify 缺少 message 应返回 2'
}

Test 'allow add 缺少参数返回 2' {
    $result = Invoke-Tool $cli @('allow', 'add')
    Assert-Equal 2 $result.ExitCode 'allow add 缺少参数应返回 2'
}

Test 'allow 未知子命令返回 2' {
    $result = Invoke-Tool $cli @('allow', 'frobnicate')
    Assert-Equal 2 $result.ExitCode 'allow 未知子命令应返回 2'
}

Test '服务未运行时 ping 返回 3' {
    # 前提：测试环境里没有运行中的 UIA-UF 服务。
    $result = Invoke-Tool $cli @('ping')
    if ($result.ExitCode -eq 0) {
        throw '检测到服务正在运行，本测试要求服务处于停止状态'
    }
    Assert-Equal 3 $result.ExitCode '服务不可用时应返回 3'
}

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

Test 'allow add 缺少 --sha256 返回 2' {
    $result = Invoke-Tool $cli @('allow', 'add', $cli)
    Assert-Equal 2 $result.ExitCode '缺少预期哈希应返回 2'
    Assert-Match $result.Output 'sha256' '应提示需要 --sha256'
}

Test 'hash 缺少参数返回 2' {
    $result = Invoke-Tool $cli @('hash')
    Assert-Equal 2 $result.ExitCode 'hash 缺少参数应返回 2'
}

Test 'hash 目标不存在返回 1' {
    $result = Invoke-Tool $cli @('hash', 'C:\definitely\missing.exe')
    Assert-Equal 1 $result.ExitCode 'hash 目标不存在应返回 1'
}

Test 'hash 输出 64 位十六进制' {
    $result = Invoke-Tool $cli @('hash', $cli)
    Assert-Equal 0 $result.ExitCode ("hash 失败：" + $result.Output)
    Assert-Match $result.Output.Trim() '^[0-9a-f]{64}$' 'hash 输出不是 64 位十六进制'
}

Test '管理器可启动并正常退出' {
    $manager = Join-Path $script:BinDir 'UIA-UF.Manager.exe'
    Assert-True (Test-Path $manager) "缺少 $manager"

    $output = "0" | & $manager 2>&1 | Out-String
    Assert-Equal 0 $LASTEXITCODE ("管理器退出码异常：" + $output)
    Assert-Match $output 'UIA-UF' '管理器没有输出标题'
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

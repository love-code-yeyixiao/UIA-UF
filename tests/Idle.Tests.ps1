# 闲置策略测试：到点自动禁用（只改 SCM 启动类型，不卸载任何东西）。
#
# 需要在"注册表项还没被服务收紧"的时候运行：服务一旦启动就会把
# HKLM\SOFTWARE\UIA-UF 的属主改成 SYSTEM、DACL 只留 SYSTEM 可写，之后管理员也写不进去。
# 因此本文件必须排在 Service.Tests.ps1 之前（按文件名排序即可），且要求干净的注册表状态。

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

$allow = $null
try {
    $allow = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\UIA-UF\Allow')
}
catch {
    Write-Host '  跳过：注册表项已存在且被收紧为仅 SYSTEM 可写（需要干净的机器/清理后重跑）' -ForegroundColor Yellow
    return
}

try {
    # 预置：空白名单 + 白名单 20 天没动过 + 已经提醒过 20 天。
    # 这样服务启动后的第一次评估就会进入"宽限期已过"分支，不依赖是否有可交互会话。
    $allow.SetValue('LastChanged', [UInt64]([DateTime]::UtcNow.AddDays(-20).ToFileTimeUtc()), 'QWord')
    $allow.Close()
    $allow = $null

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
    if ($null -ne $allow) {
        $allow.Close()
    }
    Invoke-Tool $service @('--uninstall') | Out-Null
}

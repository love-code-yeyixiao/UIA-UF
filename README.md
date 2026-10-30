# UIA-UF

[![build](https://github.com/love-code-yeyixiao/UIA-UF/actions/workflows/build.yml/badge.svg)](https://github.com/love-code-yeyixiao/UIA-UF/actions/workflows/build.yml)

用于支持多软件执行 UIAccess 权限提升的自动化统一管理框架。

UIA-UF 由一个以 `LocalSystem` 运行的服务和一个命令行启动器组成：服务使用 `SeTcbPrivilege`
为调用者的进程令牌置上 `TokenUIAccess`，再通过 `CreateProcessAsUser` 在用户会话的
`winsta0\default` 桌面上拉起目标程序。目标程序因此获得 UIAccess 的窗口层级与前台权限，
但**不会**被提权到更高的完整性级别。

> 该方案绕过了 Windows 对 UIAccess 的 Authenticode 签名与安全路径校验，属于非受支持的配置。
> 请只在你自己维护的软件上使用，并预期杀软 / EDR 会对此类行为保持关注。

## 目录结构

```
src/common/     协议、管道读写、Win32 小工具、SHA-256
src/service/    UIA-UF.Service.exe：SCM 入口、管道服务端、令牌与进程创建、白名单校验
src/cli/        uiauf.exe：启动器 / 状态检查 / 白名单管理
```

## 构建

需要 Visual Studio 2022（MSVC v143）与 CMake 3.21+。

```powershell
cmake --preset debug      # 或 --preset release / vs2022
cmake --build --preset debug
```

产物：`build/debug/UIA-UF.Service.exe`、`build/debug/uiauf.exe`。

CI 在 `windows-2022` 上构建 Debug 与 Release 两个配置，产物可在 Actions 页面下载；
推送 `v*` 标签会自动创建 Release 并附上 Release 版 zip。

## 安装与卸载

以**管理员**身份运行：

```powershell
.\UIA-UF.Service.exe --install     # 注册并启动服务（按需启动，不随系统自动运行）
.\UIA-UF.Service.exe --uninstall   # 停止并删除服务
.\UIA-UF.Service.exe --start
.\UIA-UF.Service.exe --stop
.\UIA-UF.Service.exe --console     # 前台调试模式
```

服务日志写入 `%ProgramData%\UIA-UF\service.log`（UTF-16LE）。

## 白名单

白名单是唯一决定"谁能拿到 UIAccess"的地方，位于 `HKLM\SOFTWARE\UIA-UF\Allow`，
只有管理员可写：

```
HKLM\SOFTWARE\UIA-UF\Allow\<条目名>
    Path   (REG_SZ)        可执行文件完整路径，必填
    Sha256 (REG_SZ, 可选)  允许的 SHA-256，缺省表示不校验哈希
```

用 CLI 管理（需要管理员权限的终端）：

```powershell
uiauf allow add "C:\Program Files\ClassIsland\ClassIsland.exe"          # 只绑定路径
uiauf allow add "C:\Program Files\ClassIsland\ClassIsland.exe" --hash   # 同时锁定哈希
uiauf allow list
uiauf allow remove ClassIsland
```

服务端在启动程序前会：解析目标文件的最终路径（消除 `..`、符号链接与短文件名）→
计算 SHA-256 → 与白名单逐条比对，只有完全匹配才继续。

## 使用

```powershell
uiauf run "C:\Program Files\ClassIsland\ClassIsland.exe"     # 启动
uiauf run .\ZiggyDraw.exe --wait                            # 等待退出并透传退出码
uiauf check 1234                                            # 查看 PID 是否真的具有 UIAccess
uiauf ping                                                  # 服务是否在线
```

`uiauf check` 直接读取目标进程的 `TokenUIAccess`，可作为验证手段：

```
PID 1234: UIAccess = yes, Elevated = no
```

## 安全模型

- **只有管理员能扩大授权范围**。白名单在 HKLM 下，普通用户无法新增条目；服务不会因为
  收到请求就放行任意程序。
- **服务不提权**。它复制的是调用者自己的令牌，完整性级别保持不变；UIAccess 只影响窗口
  层级、前台激活与 UIPI 相关行为，不改变进程的完整性级别。
- **管道 ACL**：`SYSTEM` / `Administrators` 完全控制，`INTERACTIVE` 仅可读写（用于本地
  用户启动自己被授权的程序）。请求是否执行由白名单决定，而不是由管道权限决定。
- **路径与哈希双重校验**：目标文件按最终路径解析后再计算哈希，校验通过后才用该最终路径
  启动，避免短文件名 / 目录联接造成的绕过。
- **不做静默提权**：CLI 自身以 `asInvoker` 运行，不会触发 UAC；安装/卸载服务与修改白名单
  需要用户显式使用管理员终端。

已知限制：

- 子进程的环境块通过 `CreateEnvironmentBlock` 生成，但没有调用 `LoadUserProfile`，
  首次登录尚未加载配置单元的用户可能拿到不完整的环境变量。
- 服务一次只处理一个请求（单线程），对本地启动场景足够。
- 只支持 x64 构建。

## 路线图

- [ ] `LoadUserProfile` / 更完整的环境块
- [ ] 以调用者会话为单位的实例管理
- [ ] 托盘 GUI 与"记住上次选择"
- [ ] 事件日志（替代纯文件日志）
- [ ] 协议与白名单校验的单元测试

---

贡献前请阅读 [AGENTS.md](AGENTS.md)，变更记录见 [CHANGELOG.md](CHANGELOG.md)。

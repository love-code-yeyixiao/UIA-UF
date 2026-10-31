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

## 闲置清理（捆绑安装场景）

服务首次启动时会把客户端 `uiauf.exe` 的路径、SHA-256 与最后修改时间登记到
`HKLM\SOFTWARE\UIA-UF\Setup`。当以下条件同时成立：

- 客户端最后修改时间距现在超过 **15 天**；
- 白名单为空，**或**所有条目指向的文件都已不存在（只检查存在性，不校验哈希）；
- 安装时没有指定 `--StandaloneSetup`；

服务会在检测到用户会话登录后校验客户端哈希，通过则拉起客户端弹出一条通知，提示
"15 天内不处理将自动卸载"。期间只要新增了可用条目，警告状态就会被清除。

```powershell
UIA-UF.Service.exe --install --StandaloneSetup   # 用户主动安装：跳过该检查
```

设计目的是清理"被捆绑安装、装完就没人管"的遗留：如果用户是自己装的框架，就不该被
这条规则打扰。

> 当前版本只做到**提醒**。宽限期结束后的自动卸载尚未接线，只在 `service.log` 里记录
> "宽限期已结束"，不会真的删除任何东西——破坏性动作（停服务、删服务、删注册表、删
> 二进制）需要先确定边界。

## 通知

`uiauf notify --title <标题> --message <正文>` 在当前会话弹出一条通知，服务端在闲置
提醒时就是用它。实现基于 `Shell_NotifyIcon`：未打包的 Win32 进程没有 AppUserModelID，
WinRT 吐司 API 需要先注册快捷方式；Win10/11 会把这个气泡交给系统通知中心，呈现为吐司。

## 白名单

白名单是唯一决定"谁能拿到 UIAccess"的地方，位于 `HKLM\SOFTWARE\UIA-UF\Allow`。

该键由服务在启动时创建，属主为 `SYSTEM`，DACL 受保护并收紧为 **仅 SYSTEM 可写**
（Administrators 只读）。因此**管理员也不能直接改注册表**，所有修改都必须经由服务完成：

```powershell
uiauf allow add "C:\Program Files\ClassIsland\ClassIsland.exe"
uiauf allow list
uiauf allow remove ClassIsland
```

需要服务在线，且当前终端处于提升状态（高完整性级别）；授权判断由服务端完成。

条目结构：

```
HKLM\SOFTWARE\UIA-UF\Allow\<条目名>
    Path   (REG_SZ)   可执行文件最终路径，必填
    Sha256 (REG_SZ)   必填，64 位十六进制
```

**哈希是强制项。** 缺少有效 `Sha256` 的条目一律按未授权处理（失败关闭）。条目名称取文件名
（不含扩展名）。

### 为什么要传"预期哈希"

`allow add` 先在本地算一遍 SHA-256 并随请求提交，服务端随后**独立重算**并与之比对，
不一致直接拒绝注册；最终写入注册表的是**服务端算出的哈希**。

这样做的目的是堵住"安装程序释放文件 → 登记白名单"之间的窗口：如果这段时间文件被替换，
两次观测必然不一致，注册会明确失败，而不是把一个被篡改的文件静默登记成合法条目。
即使调用方谎报哈希也没用——服务端只信自己算出来的那份。

目标程序更新后哈希会失配，需要重新执行 `allow add`。

## 使用

```powershell
uiauf run "C:\Program Files\ClassIsland\ClassIsland.exe"     # 启动
uiauf run .\ZiggyDraw.exe --wait                            # 等待退出并透传退出码
uiauf check 1234                                            # 查看 PID 是否真的具有 UIAccess
uiauf ping                                                  # 服务是否在线
```

`uiauf check` 直接读取目标进程的 `TokenUIAccess`，可作为验证手段：

```
PID 1234: UIAccess = yes, Elevated = no (IL 0x2000)
```

`Elevated` 与括号里的完整性级别来自令牌的 `TokenIntegrityLevel`（`TokenElevation` 在复制/模拟
令牌上不可靠）。

## 安全模型

- **只有 SYSTEM 能写白名单**。`Allow` 键的属主是 SYSTEM、DACL 受保护且只给 SYSTEM 写权限，
  管理员只能读；注册与删除都由服务在高完整性校验通过后执行。
- **哈希强制**。条目没有有效的 `Sha256` 即视为未授权；注册时调用方还要提交预期哈希，
  服务端独立重算并比对，不一致就拒绝写入。
- **服务不提权**。它复制的是调用者自己的令牌，完整性级别保持不变；UIAccess 只影响窗口
  层级、前台激活与 UIPI 相关行为，不改变进程的完整性级别。
- **管道 ACL**：`SYSTEM` / `Administrators` 完全控制，`INTERACTIVE` 仅可读写（用于本地
  用户启动自己被授权的程序）。请求是否执行由白名单决定，而不是由管道权限决定。
- **路径与哈希双重校验**：目标文件按最终路径解析后再计算哈希；校验通过的句柄以
  `FILE_SHARE_READ` 一直持有到进程创建完成（拒绝写入与删除），进程以 `CREATE_SUSPENDED`
  创建，比对实际映射映像的文件 ID 后才恢复线程，见下方"竞态"一节。
- **不做静默提权**：CLI 自身以 `asInvoker` 运行，不会触发 UAC；安装/卸载服务与修改白名单
  需要用户显式使用管理员终端。

### 关于竞态（TOCTOU）

"校验完再按路径启动"天然存在窗口期，本实现分三层处理：

| 竞态 | 处理 |
| --- | --- |
| 校验后把文件换掉 | 句柄以 `FILE_SHARE_READ` 持有（不允许写/删/改名），直到 `CreateProcessAsUser` 返回 |
| 把父目录整体改名重建，让路径指向另一个文件 | 进程以挂起状态创建，`QueryFullProcessImageNameW` + 文件 ID 比对通过后才 `ResumeThread`；不一致直接 `TerminateProcess` |
| 白名单里存词法路径、服务端比对最终路径 | `allow add` 存的是解析后的最终路径，两边口径一致 |

映像 section 在 `CreateProcess` 时就已建立，`ResumeThread` 之前没有任何用户代码执行，因此
"校验通过 → 恢复线程"之间再换文件也影响不到已映射的映像。

残余风险要说清楚：以上措施保证**启动的就是被校验的那个文件**，但不改变权限模型。能写目标目录的
普通用户仍然可以让服务为他启动一个"换成恶意内容"的、位于该目录的已授权程序——拿到的是
**他自己的身份 + UIAccess**，不是提权。真正危险的是管理员在提权终端里运行位于用户可写目录下的
程序（此时子进程是高完整性级别）；请只把管理员可写目录（如 `Program Files`）里的程序加入白名单，
或避免在提权场景下使用。

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

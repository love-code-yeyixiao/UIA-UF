# AGENTS.md

本文件面向在本仓库中工作的开发者与 AI 代理，说明项目边界、构建方式以及**不允许被破坏的安全契约**。
修改代码前请先读完本文。

## 项目定位

UIA-UF 让用户自己维护的桌面软件获得 `TokenUIAccess`（窗口层级 / 前台激活 / 对 UWP 与提权窗口的覆盖能力），
用于解决教育类软件"被别的软件压住"的实际问题。

非目标（明确不做）：

- 不做通用提权工具：任何形式的完整性级别提升（UAC bypass、令牌窃取到更高 IL）都不接受。
- 不做静默安装 / 静默改配置：安装服务、修改白名单都必须由管理员显式操作。
- 不绕过白名单、不提供"任意程序都可申请"的接口。

## 架构速查

| 路径 | 职责 |
| --- | --- |
| `src/common/protocol.h` | 管道名、消息类型、帧头、`LaunchResult`、载荷编解码 |
| `src/common/protocol_io.h` | 基于 OVERLAPPED 的超时读写、`SendMessage` / `ReceiveMessage` |
| `src/common/win32_helpers.h` | `UniqueHandle`、错误格式化、参数引用、路径工具 |
| `src/common/hash.h` | 文件 SHA-256（BCrypt） |
| `src/service/main.cpp` | 进程入口、`--install/--uninstall/--start/--stop/--console` |
| `src/service/service.cpp` | `ServiceMain`、SCM 控制处理、服务安装/卸载 |
| `src/service/server.cpp` | 命名管道服务端、请求分发、调用者身份与令牌获取 |
| `src/service/launcher.cpp` | 启用特权、`TokenUIAccess`、`CreateProcessAsUser` |
| `src/service/allowlist.cpp` | 最终路径解析 + 哈希校验 + 加载 HKLM 白名单 |
| `src/service/log.cpp` | `%ProgramData%\UIA-UF\service.log` |
| `src/cli/main.cpp` | `run` / `check` / `ping` / `allow` 子命令 |

一次启动请求的完整链路：

```
uiauf run
  └─ 规范化绝对路径 → 命名管道 → UIA-UF.Service
        └─ ImpersonateNamedPipeClient → 复制调用者令牌为 Primary
        └─ 白名单校验（最终路径 + SHA-256）
        └─ SeTcbPrivilege → TokenUIAccess = TRUE
        └─ CreateProcessAsUser(winsta0\default) → 返回 PID
```

## 构建与验证

```powershell
cmake --preset debug          # 或 release / vs2022
cmake --build --preset debug
```

CI（`.github/workflows/build.yml`）在 `windows-2022` 上构建 Debug 与 Release，运行
`tests/` 下的测试，并上传两个 exe；打 `v*` 标签时自动发布 zip。**本地提交前至少保证
MSVC `/W4` 无新增警告，且 `tests/Run-Tests.ps1` 通过。**

## 测试

`tests/` 下是基于 PowerShell 的测试，不依赖 Pester：

| 文件 | 内容 |
| --- | --- |
| `Run-Tests.ps1` | 入口：依次执行 `*.Tests.ps1`，汇总并设置退出码 |
| `TestHelpers.ps1` | `Test` / `Assert-*` / `Invoke-Tool`（捕获输出与退出码） |
| `Cli.Tests.ps1` | CLI 行为：help、错误参数、缺少文件、服务离线时的退出码 |
| `Service.Tests.ps1` | 集成：安装服务 → 注册 → 启动 → `check` → 篡改拒绝 → 删除 → 卸载 |

```powershell
pwsh -File tests\Run-Tests.ps1 -BinDir build\release
pwsh -File tests\Run-Tests.ps1 -BinDir out -SkipServiceTests   # 无法装服务的环境
```

- 服务端集成测试需要管理员权限；不满足时会**整体跳过**而不是失败。
- 脚本必须保存为 **UTF-8 with BOM**，否则 Windows PowerShell 5.1 会把中文按 ANSI 解析，
  直接报语法错误。
- 集成测试会真的创建并启动服务、拉起进程、最后在 `finally` 里卸载，不要在正在使用的
  机器上直接跑。

只有 `cl.exe`、没有可用 CMake 环境时的等价编译命令：

```powershell
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /permissive- /I src /DUNICODE /D_UNICODE ^
   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /MTd ^
   src\service\*.cpp /Fe:out\UIA-UF.Service.exe /link advapi32.lib bcrypt.lib user32.lib userenv.lib
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /permissive- /I src /DUNICODE /D_UNICODE ^
   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /MTd ^
   src\cli\*.cpp /Fe:out\uiauf.exe /link advapi32.lib bcrypt.lib
```

构建产物为静态 CRT，不依赖 VC++ 运行库。

### 已知环境问题

- 某些受限环境（沙箱 / 未启用 8.3 短文件名的卷）里，CMake 会把含空格的编译器路径转换成不可访问的短路径
  （如 `D:\MICROS~1\2022\COMMUN~C.nPS`），导致 `project()` 阶段失败。此时请改用上面的 `cl` 命令，
  或在无空格的路径下安装 CMake + Ninja。
- 沙箱内通常无法启动服务、也拿不到 `SeTcbPrivilege`：`SetTokenInformation(TokenUIAccess)` 会返回
  `ERROR_PRIVILEGE_NOT_HELD (1314)`。这是预期结果，不代表代码有 bug，请在虚拟机/实机验证。

## 安全契约（不要破坏）

1. **白名单是唯一授权来源。** 服务绝不能启动未经 `ResolveAllowedExecutable` 批准的二进制。
   唯一例外是闲置提醒：服务可以启动 `Setup\ClientPath` 中登记、且哈希校验通过的客户端
   `notify` 子命令，不要把这个例外扩大到任意路径。
2. **哈希强制。** 条目缺少有效 `Sha256` 一律按未授权处理；注册必须由调用方提交预期哈希，
   服务端重算比对后才写入，且写入的是服务端算出的值。
3. **白名单只能经服务修改。** 键的 ACL 仅 SYSTEM 可写，`allow add/remove` 必须走管道；
   服务侧要校验调用者为高完整性且属于 Administrators，不要给 CLI 直接写注册表的口子。
4. **不提权。** 只能复制调用者自己的令牌，不得修改完整性级别、不得跨会话取令牌。
5. **按最终路径校验，并在启动时再次钉住映像。** 用 `GetFinalPathNameByHandleW` 消除短文件名 /
   目录联接；校验用的句柄必须以 `FILE_SHARE_READ` 持有到 `CreateProcessAsUser` 返回，进程必须以
   `CREATE_SUSPENDED` 创建，比对实际映射映像的文件 ID（卷序列号 + file index）后才能 `ResumeThread`。
   不要把这些环节"简化"成先关句柄、再按路径启动。
6. **管道 ACL 不放宽。** `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`，只允许交互用户读写，
   不要加 `WD`、`AN` 或 `Everyone`。
7. **CLI 保持 `asInvoker`。** 不请求管理员清单、不自动提权；需要管理员的操作由服务端
   校验后拒绝，并返回明确错误。
8. **不记录敏感信息。** 日志里只写调用者账号、最终路径、PID 与错误码。

## 代码约定

- C++20，MSVC `/W4`；不使用异常做控制流，跨 Win32 边界的函数统一返回 `DWORD`（Win32 错误码）或 `bool`。
- 资源一律用 RAII 包装（`UniqueHandle` / `UniqueRegKey`），禁止裸 `CloseHandle` 分支散落。
- 全程宽字符（`std::wstring` / UTF-16），不做 ANSI 转换。
- `src/common` 只放头文件，不引入第三方依赖；新增依赖需要先在 PR 里说明理由。
- 面向用户的输出走 `%ls` + `_O_U8TEXT`（见 `EnableUnicodeConsole`），保证重定向到文件时仍是合法 UTF-8。
- 4 空格缩进，单行不超过约 110 列，保持与现有文件一致。

## 协议变更

`protocol.h` 是服务端与 CLI 的唯一契约：

- 任何线格式变化都必须同时修改两端，并把 `kProtocolVersion` 加一；旧版本收到新协议应返回
  `ERROR_REVISION_MISMATCH` 而不是崩溃。
- 载荷上限 `kMaxPayloadBytes`（64 KiB）不得随意放大；超限即视为非法请求。
- 新增消息类型时同步更新 `MessageType` 与 `ServiceServer::HandleClient` 的 `switch`。

## 白名单与数据

```
HKLM\SOFTWARE\UIA-UF\Allow\<条目名>      属主 SYSTEM，受保护 DACL，仅 SYSTEM 可写（BA 只读）
    Path   (REG_SZ)        必填，可执行文件的最终路径
    Sha256 (REG_SZ)        必填，64 位十六进制；缺失即视为未授权
HKLM\SOFTWARE\UIA-UF\Allow
    LastChanged (REG_QWORD)  白名单最后一次增删的时间（FILETIME），闲置清理用它计时
HKLM\SOFTWARE\UIA-UF\Setup               安装登记，仅服务写入
    ClientPath       (REG_SZ)     登记的客户端路径
    ClientSha256     (REG_SZ)     安装时的客户端哈希
    InstalledAt      (REG_QWORD)  首次登记时间（FILETIME）
    Standalone       (REG_DWORD)  1 = 独立安装，跳过闲置清理
    WarnedAt         (REG_QWORD)  上次闲置提醒时间（FILETIME）
%ProgramData%\UIA-UF\service.log   UTF-16LE，带 BOM
```

- 注册表读写需带 `KEY_WOW64_64KEY`，避免 WOW64 重定向。
- 键的 ACL 由服务在启动时通过 `EnsureAllowListKey` 收紧（`SeRestorePrivilege` 改属主），
  **管理员不能再直接写**；`allow add/remove` 走管道交给服务执行，服务侧要求调用者是
  高完整性的 Administrators 成员。
- "预期哈希"不是信任来源：调用方提交的哈希只用于和服务端重算的结果比对，写入注册表的是
  服务端算出的值。这样"释放文件 → 登记"之间被换文件会注册失败，而不是被静默登记。
- 本地管理员理论上仍可夺取属主后改写该键——ACL 收紧提高的是门槛，不是对本地管理员的
  权限边界，不要把这条当成防提权保证。

## 闲置清理策略

- 仅当 `Standalone=0` 时生效。白名单距上次修改（`Allow\LastChanged`，从未修改则用
  `InstalledAt`）超过 15 天，且白名单"无可用条目"（为空，或全部条目指向的文件都不存在）
  时才触发——只查存在性，不查哈希，这是卫生检查而不是授权判断。
- 触发时先校验 `ClientPath` 的哈希，通过才启动 `notify` 通知；不通过只记日志，绝不提醒。
- 提醒后再过 15 天仍未处理，服务会把自己的 SCM 启动类型改为 `DISABLED` 并停止。
  **这是有意的非破坏性选择**：不删服务、不删文件、不删注册表，`--install` 即可恢复
  （恢复时会清除 `WarnedAt`/`AutoDisabledAt`，重新计一轮提醒）。
- 被禁用期间，CLI 会检测到 `SERVICE_DISABLED` 并提示恢复/卸载方式，每天最多弹一次通知
  （`HKCU\Software\UIA-UF\LastDisabledNotice` 限流）。
- 阈值可通过 `Setup\IdleDays` / `Setup\WarnDays`（DWORD，默认 15）调整，测试用它压缩时间。
- 只要新增了可用条目，`WarnedAt` 会被清除，避免以后再次闲置时反而不再提醒。

## 验证清单

改动涉及令牌 / 启动 / 白名单时，在虚拟机里跑一遍：

```powershell
.\UIA-UF.Service.exe --install
$hash = .\uiauf.exe hash "C:\Windows\System32\notepad.exe"
.\uiauf.exe allow add "C:\Windows\System32\notepad.exe" --sha256 $hash
.\uiauf.exe allow list               # Sha256 一列必须有值
.\uiauf.exe ping
.\uiauf.exe run C:\Windows\System32\notepad.exe
.\uiauf.exe check <PID>            # 期望 UIAccess = yes, Elevated 与调用者一致
.\uiauf.exe allow remove notepad
.\UIA-UF.Service.exe --uninstall
```

同时确认：未在白名单中的程序被拒绝；篡改哈希后被拒绝；**手工删掉某个条目的 `Sha256` 后该程序
被拒绝**（失败关闭）；**在服务运行期间把目标文件换成另一个 exe 后被拒绝**
（`CreateProcessAsUser` 前替换应因共享冲突打开失败，之后替换应被映像 ID 校验拦下）；
`--uninstall` 后管道不再存在。

闲置策略单独验一次：把 `Setup\ClientLastWrite` 改成 20 天前、保持白名单为空，重启服务后
在用户会话应收到通知，`WarnedAt` 被写入；把 `Standalone` 设为 1 后不再触发；加一个可用
条目后 `WarnedAt` 被清除。

## 文档与版本

- 面向用户的说明写在 `README.md`；给代理/贡献者的规则写在本文件。
- 每个用户可见的改动都要在 `CHANGELOG.md` 的 `[Unreleased]` 下追加条目，发布时改为版本号 + 日期。
- 版本号在 `CMakeLists.txt` 的 `project(UIA-UF VERSION ...)` 中，遵循 SemVer；发布时打 `vMAJOR.MINOR.PATCH` 标签，
  CI 会基于该标签生成 Release。
- 不提交 `build/`、`out/`、`.vs/`、`*.user`。

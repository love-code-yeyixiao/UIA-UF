# Changelog

本项目的所有重要变更都记录在此文件中。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。


## [Unreleased]

### Added

- `uiauf notify --title <标题> --message <正文>`：在当前会话弹通知（`Shell_NotifyIcon`，
  Win10/11 由系统通知中心呈现）。
- 安装登记：服务首次启动时把客户端路径与 SHA-256 写入
  `HKLM\SOFTWARE\UIA-UF\Setup`；`--install` 时强制重新登记。
- 白名单修改时间：`Allow\LastChanged` 在条目增删时刷新，服务启动不会重置。
- 闲置清理提醒：白名单超过 15 天未修改、且没有可用条目时，在用户会话登录后校验客户端
  哈希并弹通知；`--install --StandaloneSetup` 可关闭该检查。
- `tests/`：基于 PowerShell 的测试（不依赖 Pester），含 CLI 行为测试与服务端集成测试；
  `Run-Tests.ps1 -SkipServiceTests` 可在受限环境只跑 CLI 部分。
- CI 新增 `tests` 作业：下载 Release 产物后执行完整测试，标签发布依赖它通过。

### Changed

- 白名单管理的管理员校验改为枚举令牌组（`SE_GROUP_ENABLED`），不再使用
  `CheckTokenMembership`——后者在传入句柄时要求模拟令牌，在提升终端下会返回 1309。
- `--install` 现在会先停止服务再用安装模式参数重新启动，确保 `--StandaloneSetup` 生效。
- `uiauf check` 对已经退出的 PID 给出明确提示，而不是直接输出 Win32 错误码。
- 白名单 `Sha256` 改为**强制项**：缺少有效哈希的条目一律按未授权处理，不再接受"只绑路径"。
- 注册白名单必须提交**预期哈希**：CLI 本地计算并随请求发送，服务端独立重算并比对，
  不一致直接拒绝；最终写入注册表的是服务端算出的哈希。
- 白名单的增删改全部改走命名管道，由服务（LocalSystem）执行，CLI 不再直接写注册表。
- `uiauf check` 改为显示令牌完整性级别（`TokenElevation` 在复制/模拟令牌上不可靠）。
- 协议版本升到 2：新增 `AllowAdd` / `AllowRemove` / `AllowList` / `AllowResult` / `AllowListResult`。

### Security

- `HKLM\SOFTWARE\UIA-UF` 与 `...\Allow` 的属主改为 `SYSTEM`，DACL 受保护、仅 SYSTEM 可写，
  Administrators 只读；服务启动时自动收紧已存在的键。
- 关闭"哈希校验"与"启动"之间的 TOCTOU 窗口：校验句柄改为 `FILE_SHARE_READ` 并持有到
  `CreateProcessAsUser` 返回；进程以 `CREATE_SUSPENDED` 创建，比对实际映射映像的文件 ID
  （卷序列号 + file index）通过后才 `ResumeThread`，不一致则终止进程。
- 白名单条目改为写入解析后的最终路径，修正 `allow add` 与服务端比对口径不一致导致的
  "加了却用不了"（失败关闭，非绕过）。

## [0.1.0] - 2026-10-30

首个可运行的 MVP：SYSTEM 服务 + 命令行启动器 + 白名单。

### Added

- `UIA-UF.Service`：以 `LocalSystem` 运行的自定义服务，按需启动，可通过
  `--install` / `--uninstall` / `--start` / `--stop` / `--console` 管理。
- 命名管道服务端（`\\.\pipe\UIA-UF.v1`），自定义帧协议与 64 KiB 载荷上限，读写均带超时。
- 令牌处理：`ImpersonateNamedPipeClient` 复制调用者主令牌，启用 `SeTcbPrivilege` 后置位
  `TokenUIAccess`，再用 `CreateProcessAsUser` 在 `winsta0\default` 上启动目标程序。
- 白名单：`HKLM\SOFTWARE\UIA-UF\Allow\<名称>`，支持仅绑定路径或同时锁定 SHA-256；
  校验基于 `GetFinalPathNameByHandleW` 的最终路径，并用最终路径启动。
- `uiauf` CLI：`run`（含 `--wait`）、`check`、`ping`、`allow list|add|remove`。
- 服务日志：`%ProgramData%\UIA-UF\service.log`（UTF-16LE）。
- CMake 构建（静态 CRT）、`CMakePresets.json`（Ninja Debug/Release、VS 2022）。
- GitHub Actions：Debug/Release 双配置构建、冒烟测试、产物上传，`v*` 标签自动发布。
- 文档：`README.md`（使用说明与安全模型）、`AGENTS.md`（贡献与安全契约）。

### Security

- 服务不提升完整性级别，只复制调用者令牌并附加 UIAccess 标志。
- 白名单位于 HKLM，仅管理员可扩大授权范围；管道 ACL 对交互用户只开放读写。
- 该实现绕过 Windows 对 UIAccess 的 Authenticode 签名与安全路径校验，属于非受支持的配置，
  请只在自有软件上使用。
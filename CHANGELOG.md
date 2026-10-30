# Changelog

本项目的所有重要变更都记录在此文件中。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

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

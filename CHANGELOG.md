# Changelog

本项目的所有重要变更都记录在此文件中。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [0.0.1] - 2026-10-31

首个公开版本。

### 提供的能力

- **UIA-UF.Service**：以 `LocalSystem` 运行、按需启动的服务；`--install` / `--uninstall` /
  `--start` / `--stop` / `--console`，`--StandaloneSetup` 表示用户主动安装。
- **uiauf**：`run` / `check` / `ping` / `hash` / `notify` / `allow list|add|remove`。
- 命名管道（`\\.\pipe\UIA-UF.v1`）自定义帧协议，读写带超时，载荷上限 64 KiB。
- 启动链路：复制调用者令牌 → `TokenUIAccess` → `CreateProcessAsUser`（`winsta0\default`）；
  进程以挂起状态创建，比对实际映射映像的文件 ID 后才恢复线程。
- 白名单：`HKLM\SOFTWARE\UIA-UF\Allow\<名称>`，`Path` + 强制 `Sha256`；键由服务收紧为仅
  SYSTEM 可写，增删改经管道并要求高完整性管理员；注册时必须提交预期哈希，服务端独立重算
  比对后才写入。
- 闲置清理：白名单 15 天未修改且没有可用条目时提醒；宽限期后再过 15 天把服务在 SCM 中
  置为 `DISABLED`（可逆，不删除任何东西）。`--StandaloneSetup` 可关闭。
- `uiauf notify` 通过 `Shell_NotifyIcon` 发通知，Win10/11 由系统通知中心呈现。
- 构建：CMake + 静态 CRT；exe 带 `VERSIONINFO`（版本号取自 `project(VERSION)`）；
  GitHub Actions 构建 / 测试 / 标签发布；`tests/` 为 PowerShell 测试。

### 说明

- 绕过 Windows 对 UIAccess 的 Authenticode 签名与安全路径校验，属于非受支持的配置，
  请只在自有软件上使用，并预期杀软 / EDR 关注。
- 服务不提升完整性级别：只复制调用者令牌并附加 UIAccess 标志。
- UIAccess 只影响窗口层级、前台激活与 UIPI；窗口是否置顶仍由目标程序自己决定
  （`SetWindowPos` / `SetWindowBand`），本框架只负责令牌。

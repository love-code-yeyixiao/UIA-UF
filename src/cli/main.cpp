#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <string_view>
#include <vector>

#include "common/hash.h"
#include "common/protocol.h"
#include "common/protocol_io.h"
#include "common/win32_helpers.h"

namespace {

using uiauf::UniqueHandle;

void EnableUnicodeConsole() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
}

void PrintUsage() {
    std::fwprintf(stderr,
                  L"uiauf.exe -- UIAccess 启动器\n"
                  L"\n"
                  L"用法:\n"
                  L"  uiauf run <可执行文件> [参数...] [--wait]   以 UIAccess 启动程序\n"
                  L"  uiauf check <PID>                          查看进程是否具有 UIAccess\n"
                  L"  uiauf ping                                 检查服务是否在线\n"
                  L"  uiauf allow list                           列出白名单\n"
                  L"  uiauf allow add <文件>                     注册白名单条目（管理员，自动锁定哈希）\n"
                  L"  uiauf allow remove <名称>                  删除白名单条目（管理员）\n"
                  L"  uiauf help                                 显示本帮助\n"
                  L"\n"
                  L"白名单位于 HKLM\\SOFTWARE\\UIA-UF\\Allow\\<名称>，ACL 仅允许 SYSTEM 写入，\n"
                  L"因此所有修改都通过服务完成，需要服务在线且当前终端已提权：\n"
                  L"  Path   (REG_SZ)       可执行文件最终路径\n"
                  L"  Sha256 (REG_SZ)       强制项，必须与文件内容一致\n"
                  L"\n"
                  L"条目名称取文件名（不含扩展名）。注册时会同时提交预期哈希，服务端重新计算并比对，\n"
                  L"不一致则拒绝写入。--hash 参数保留兼容，现已默认启用。\n");
}

std::wstring ToFullPath(const std::wstring& path) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size()) {
        return path;
    }
    return std::wstring(buffer.data(), length);
}

// Stores the dereferenced path, which is what the service compares against.
std::wstring ResolveFinalPath(const std::wstring& path) {
    UniqueHandle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return path;
    }
    const std::wstring finalPath = uiauf::FinalPathOf(file.get());
    return finalPath.empty() ? path : finalPath;
}

bool ReadFileSha256(const std::wstring& path, std::wstring& digest, DWORD& error) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    error = uiauf::ComputeSha256Hex(file.get(), digest);
    return error == ERROR_SUCCESS;
}

UniqueHandle ConnectToService(DWORD& error) {
    error = ERROR_SUCCESS;

    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE pipe = CreateFileW(uiauf::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return uiauf::WrapHandle(pipe);
        }

        error = GetLastError();
        if (error == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(uiauf::kPipeName, 200);
            continue;
        }
        if (error == ERROR_FILE_NOT_FOUND) {
            Sleep(100);
            continue;
        }
        break;
    }

    return {};
}

int ReportServiceUnavailable(DWORD error) {
    std::fwprintf(stderr,
                  L"无法连接到 UIA-UF 服务 (%ls)。\n"
                  L"请确认服务已安装并正在运行：\n"
                  L"  UIA-UF.Service.exe --install\n",
                  uiauf::FormatWin32Error(error).c_str());
    return 3;
}

std::wstring JoinArguments(const std::vector<std::wstring>& arguments, size_t first) {
    std::wstring commandLine;
    for (size_t i = first; i < arguments.size(); ++i) {
        if (!commandLine.empty()) {
            commandLine.push_back(L' ');
        }
        commandLine += uiauf::QuoteArgument(arguments[i]);
    }
    return commandLine;
}

int RunCommand(const std::vector<std::wstring>& positional, bool waitForExit) {
    if (positional.size() < 2) {
        std::fwprintf(stderr, L"run: 缺少可执行文件路径。\n");
        return 2;
    }

    // The service runs with its own working directory, so relative paths have
    // to be resolved on this side.
    const std::wstring executable = ToFullPath(positional[1]);
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"找不到文件: %ls\n", executable.c_str());
        return 2;
    }

    const std::wstring arguments = JoinArguments(positional, 2);

    DWORD error = ERROR_SUCCESS;
    UniqueHandle pipe = ConnectToService(error);
    if (!pipe) {
        return ReportServiceUnavailable(error);
    }

    const std::vector<std::byte> payload = uiauf::MakeLaunchPayload(executable, arguments);
    error = uiauf::SendMessage(pipe.get(), uiauf::MessageType::Launch, payload.data(),
                               static_cast<uint32_t>(payload.size()));
    if (error != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"发送请求失败: %ls\n", uiauf::FormatWin32Error(error).c_str());
        return 1;
    }

    uiauf::MessageType type = uiauf::MessageType::LaunchResult;
    std::vector<std::byte> response;
    error = uiauf::ReceiveMessage(pipe.get(), type, response);
    if (error != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"读取响应失败: %ls\n", uiauf::FormatWin32Error(error).c_str());
        return 1;
    }
    if (type != uiauf::MessageType::LaunchResult || response.size() != sizeof(uiauf::LaunchResult)) {
        std::fwprintf(stderr, L"服务返回了无法识别的响应。\n");
        return 1;
    }

    uiauf::LaunchResult result{};
    std::memcpy(&result, response.data(), sizeof(result));

    if (result.status != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"启动失败 (%ls): %ls\n", uiauf::FormatWin32Error(result.status).c_str(),
                      result.message);
        return 1;
    }

    std::wprintf(L"已启动，PID = %lu\n", result.childPid);

    if (!waitForExit) {
        return 0;
    }

    UniqueHandle child(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, result.childPid));
    if (!child) {
        std::fwprintf(stderr, L"无法等待子进程: %ls\n", uiauf::FormatWin32Error(GetLastError()).c_str());
        return 1;
    }

    WaitForSingleObject(child.get(), INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(child.get(), &exitCode);
    return static_cast<int>(exitCode);
}

int CheckCommand(const std::vector<std::wstring>& positional) {
    if (positional.size() < 2) {
        std::fwprintf(stderr, L"check: 缺少 PID。\n");
        return 2;
    }

    wchar_t* end = nullptr;
    const unsigned long pid = std::wcstoul(positional[1].c_str(), &end, 10);
    if (end == positional[1].c_str() || *end != L'\0' || pid == 0) {
        std::fwprintf(stderr, L"check: 无效的 PID。\n");
        return 2;
    }

    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        std::fwprintf(stderr, L"无法打开进程 %lu: %ls\n", pid,
                      uiauf::FormatWin32Error(GetLastError()).c_str());
        return 1;
    }

    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &rawToken)) {
        std::fwprintf(stderr, L"无法打开进程令牌: %ls\n", uiauf::FormatWin32Error(GetLastError()).c_str());
        return 1;
    }
    UniqueHandle token(rawToken);

    DWORD returned = 0;
    DWORD uiAccess = 0;
    const bool hasUiAccess = GetTokenInformation(token.get(), TokenUIAccess, &uiAccess, sizeof(uiAccess),
                                                 &returned) != FALSE &&
                             uiAccess != 0;

    DWORD rid = 0;
    const bool haveRid = uiauf::IntegrityLevelRid(token.get(), rid);
    const bool elevated = haveRid && rid >= SECURITY_MANDATORY_HIGH_RID;

    std::wprintf(L"PID %lu: UIAccess = %ls, Elevated = %ls (IL 0x%lx)\n", pid,
                 hasUiAccess ? L"yes" : L"no", elevated ? L"yes" : L"no", rid);
    return hasUiAccess ? 0 : 1;
}

int PingCommand() {
    DWORD error = ERROR_SUCCESS;
    UniqueHandle pipe = ConnectToService(error);
    if (!pipe) {
        return ReportServiceUnavailable(error);
    }

    error = uiauf::SendMessage(pipe.get(), uiauf::MessageType::Ping, nullptr, 0);
    if (error != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"发送请求失败: %ls\n", uiauf::FormatWin32Error(error).c_str());
        return 1;
    }

    uiauf::MessageType type = uiauf::MessageType::Pong;
    std::vector<std::byte> response;
    error = uiauf::ReceiveMessage(pipe.get(), type, response);
    if (error != ERROR_SUCCESS || type != uiauf::MessageType::Pong ||
        response.size() != sizeof(uiauf::PingResult)) {
        std::fwprintf(stderr, L"服务没有正确响应。\n");
        return 1;
    }

    uiauf::PingResult result{};
    std::memcpy(&result, response.data(), sizeof(result));
    std::wprintf(L"服务在线，PID = %lu，协议版本 = %lu\n", result.servicePid, result.protocolVersion);
    return 0;
}

int SendAllowRequest(const wchar_t* label, uiauf::MessageType requestType,
                     const std::vector<std::byte>& payload) {
    DWORD error = ERROR_SUCCESS;
    UniqueHandle pipe = ConnectToService(error);
    if (!pipe) {
        return ReportServiceUnavailable(error);
    }

    error = uiauf::SendMessage(pipe.get(), requestType, payload.data(), static_cast<uint32_t>(payload.size()));
    if (error != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"发送请求失败: %ls\n", uiauf::FormatWin32Error(error).c_str());
        return 1;
    }

    uiauf::MessageType type = uiauf::MessageType::AllowResult;
    std::vector<std::byte> response;
    error = uiauf::ReceiveMessage(pipe.get(), type, response);
    if (error != ERROR_SUCCESS || type != uiauf::MessageType::AllowResult ||
        response.size() != sizeof(uiauf::OperationResult)) {
        std::fwprintf(stderr, L"服务返回了无法识别的响应。\n");
        return 1;
    }

    uiauf::OperationResult result{};
    std::memcpy(&result, response.data(), sizeof(result));
    if (result.status != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"%ls失败 (%ls): %ls\n", label, uiauf::FormatWin32Error(result.status).c_str(),
                      result.message);
        return 1;
    }

    std::wprintf(L"%ls\n", result.message);
    return 0;
}

int AllowListCommand() {
    DWORD error = ERROR_SUCCESS;
    UniqueHandle pipe = ConnectToService(error);
    if (!pipe) {
        return ReportServiceUnavailable(error);
    }

    error = uiauf::SendMessage(pipe.get(), uiauf::MessageType::AllowList, nullptr, 0);
    if (error != ERROR_SUCCESS) {
        std::fwprintf(stderr, L"发送请求失败: %ls\n", uiauf::FormatWin32Error(error).c_str());
        return 1;
    }

    uiauf::MessageType type = uiauf::MessageType::AllowListResult;
    std::vector<std::byte> response;
    error = uiauf::ReceiveMessage(pipe.get(), type, response);
    if (error != ERROR_SUCCESS || type != uiauf::MessageType::AllowListResult) {
        std::fwprintf(stderr, L"服务返回了无法识别的响应。\n");
        return 1;
    }

    std::vector<std::wstring> parts;
    if (!uiauf::ParseStringPayload(response.data(), response.size(), parts) || parts.size() % 3 != 0) {
        std::fwprintf(stderr, L"白名单响应格式错误。\n");
        return 1;
    }

    if (parts.empty()) {
        std::wprintf(L"白名单为空。\n");
        return 0;
    }

    std::wprintf(L"%-24ls %-64ls %ls\n", L"名称", L"路径", L"SHA-256");
    for (size_t index = 0; index + 2 < parts.size(); index += 3) {
        std::wprintf(L"%-24ls %-64ls %ls\n", parts[index].c_str(), parts[index + 1].c_str(),
                     parts[index + 2].empty() ? L"(缺失，已失效)" : parts[index + 2].c_str());
    }
    std::wprintf(L"\n共 %zu 个条目。\n", parts.size() / 3);
    return 0;
}

int AllowAddCommand(const std::vector<std::wstring>& positional) {
    if (positional.size() < 3) {
        std::fwprintf(stderr, L"allow add: 缺少文件路径。\n");
        return 2;
    }

    const std::wstring path = ResolveFinalPath(ToFullPath(positional[2]));
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"找不到文件: %ls\n", path.c_str());
        return 2;
    }

    std::wstring digest;
    DWORD error = ERROR_SUCCESS;
    if (!ReadFileSha256(path, digest, error)) {
        std::fwprintf(stderr, L"计算 SHA-256 失败: %ls\n", uiauf::FormatWin32Error(error).c_str());
        return 1;
    }

    std::wprintf(L"注册 %ls\n  预期 SHA-256 = %ls\n", path.c_str(), digest.c_str());
    return SendAllowRequest(L"注册白名单条目", uiauf::MessageType::AllowAdd,
                            uiauf::MakeStringPayload({path, digest}));
}

int AllowRemoveCommand(const std::vector<std::wstring>& positional) {
    if (positional.size() < 3) {
        std::fwprintf(stderr, L"allow remove: 缺少条目名称。\n");
        return 2;
    }

    return SendAllowRequest(L"删除白名单条目", uiauf::MessageType::AllowRemove,
                            uiauf::MakeStringPayload({positional[2]}));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    EnableUnicodeConsole();

    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    const std::wstring_view command = argv[1];
    std::vector<std::wstring> positional;
    bool waitForExit = false;

    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--wait") == 0) {
            waitForExit = true;
            continue;
        }
        if (std::wcscmp(argv[i], L"--hash") == 0) {
            // Kept for compatibility: hashing is mandatory now.
            continue;
        }
        positional.emplace_back(argv[i]);
    }

    if (command == L"run") {
        return RunCommand(positional, waitForExit);
    }
    if (command == L"check") {
        return CheckCommand(positional);
    }
    if (command == L"ping") {
        return PingCommand();
    }
    if (command == L"allow") {
        if (positional.size() < 2) {
            PrintUsage();
            return 2;
        }
        if (positional[1] == L"list") {
            return AllowListCommand();
        }
        if (positional[1] == L"add") {
            return AllowAddCommand(positional);
        }
        if (positional[1] == L"remove" || positional[1] == L"delete") {
            return AllowRemoveCommand(positional);
        }
        std::fwprintf(stderr, L"allow: 未知子命令 %ls\n", positional[1].c_str());
        return 2;
    }
    if (command == L"help" || command == L"--help" || command == L"-h") {
        PrintUsage();
        return 0;
    }

    std::fwprintf(stderr, L"未知命令: %ls\n\n", argv[1]);
    PrintUsage();
    return 2;
}

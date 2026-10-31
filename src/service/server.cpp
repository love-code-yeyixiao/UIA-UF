#include "service/server.h"

#include <sddl.h>

#include <string>

#include "common/protocol_io.h"
#include "service/allowlist.h"
#include "service/launcher.h"
#include "service/log.h"

namespace uiauf {
namespace {

// SYSTEM and Administrators get full control; interactive users may only
// connect and exchange messages. Whether a request is honoured is decided by
// the allow list, not by the pipe ACL.
constexpr wchar_t kPipeSecuritySddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";

class ImpersonationScope {
public:
    explicit ImpersonationScope(bool active) : active_(active) {}
    ~ImpersonationScope() {
        Revert();
    }
    ImpersonationScope(const ImpersonationScope&) = delete;
    ImpersonationScope& operator=(const ImpersonationScope&) = delete;

    void Revert() {
        if (active_) {
            RevertToSelf();
            active_ = false;
        }
    }

private:
    bool active_;
};

void SetResultMessage(LaunchResult& result, const std::wstring& text) {
    wcsncpy_s(result.message, _countof(result.message), text.c_str(), _TRUNCATE);
}

void SetResultMessage(OperationResult& result, const std::wstring& text) {
    wcsncpy_s(result.message, _countof(result.message), text.c_str(), _TRUNCATE);
}

// Checks that the caller's token is a high integrity member of Administrators.
bool IsElevatedAdministratorToken(HANDLE token, DWORD& error) {
    DWORD rid = 0;
    if (!IntegrityLevelRid(token, rid)) {
        error = GetLastError();
        LogLine(L"读取调用者完整性级别失败" + FormatWin32Error(error));
        return false;
    }
    if (rid < SECURITY_MANDATORY_HIGH_RID) {
        wchar_t text[96]{};
        swprintf_s(text, L"调用者完整性级别不足：%lu (0x%lx)", rid, rid);
        LogLine(text);
        error = ERROR_ACCESS_DENIED;
        return false;
    }

    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0,
                                  0, 0, 0, &administrators)) {
        error = GetLastError();
        LogLine(L"构建 Administrators SID 失败" + FormatWin32Error(error));
        return false;
    }

    BOOL member = FALSE;
    const BOOL checked = CheckTokenMembership(token, administrators, &member);
    FreeSid(administrators);

    if (!checked) {
        error = GetLastError();
        LogLine(L"CheckTokenMembership 失败" + FormatWin32Error(error));
        return false;
    }
    if (!member) {
        LogLine(L"调用者不属于 Administrators。");
        error = ERROR_ACCESS_DENIED;
        return false;
    }

    error = ERROR_SUCCESS;
    return true;
}

} // namespace

ServiceServer::~ServiceServer() {
    RequestStop();
    Wait();
}

bool ServiceServer::Start() {
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent_) {
        LogError(L"CreateEvent(stop)", GetLastError());
        return false;
    }

    thread_ = std::thread([this] { Run(); });
    return true;
}

void ServiceServer::RequestStop() {
    if (stopEvent_) {
        SetEvent(stopEvent_.get());
    }
}

void ServiceServer::Wait() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ServiceServer::Run() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kPipeSecuritySddl, SDDL_REVISION_1, &descriptor,
                                                             nullptr)) {
        LogError(L"ConvertStringSecurityDescriptorToSecurityDescriptor", GetLastError());
        return;
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;

    UniqueHandle connectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!connectEvent) {
        LogError(L"CreateEvent(connect)", GetLastError());
        LocalFree(descriptor);
        return;
    }

    LogLine(L"正在监听 " + std::wstring(kPipeName));

    while (WaitForSingleObject(stopEvent_.get(), 0) != WAIT_OBJECT_0) {
        UniqueHandle pipe(CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                           PIPE_UNLIMITED_INSTANCES, kMaxPayloadBytes + sizeof(MessageHeader),
                                           kMaxPayloadBytes + sizeof(MessageHeader), 0, &attributes));
        if (pipe.get() == INVALID_HANDLE_VALUE) {
            LogError(L"CreateNamedPipe", GetLastError());
            Sleep(1000);
            continue;
        }

        ResetEvent(connectEvent.get());
        OVERLAPPED overlapped{};
        overlapped.hEvent = connectEvent.get();

        const BOOL connected = ConnectNamedPipe(pipe.get(), &overlapped);
        DWORD error = connected ? ERROR_SUCCESS : GetLastError();

        if (!connected && error == ERROR_IO_PENDING) {
            const HANDLE waits[] = {stopEvent_.get(), connectEvent.get()};
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) {
                // Stop requested while waiting for a client.
                CancelIoEx(pipe.get(), &overlapped);
                break;
            }
            if (wait != WAIT_OBJECT_0 + 1) {
                LogError(L"WaitForMultipleObjects", GetLastError());
                continue;
            }

            DWORD transferred = 0;
            if (!GetOverlappedResult(pipe.get(), &overlapped, &transferred, FALSE) &&
                GetLastError() != ERROR_PIPE_CONNECTED) {
                continue;
            }
        } else if (!connected && error != ERROR_PIPE_CONNECTED) {
            LogError(L"ConnectNamedPipe", error);
            continue;
        }

        HandleClient(pipe.get());
        FlushFileBuffers(pipe.get());
        DisconnectNamedPipe(pipe.get());
    }

    LocalFree(descriptor);
    LogLine(L"监听已停止。");
}

void ServiceServer::HandleClient(HANDLE pipe) {
    MessageType type = MessageType::Ping;
    std::vector<std::byte> payload;
    const DWORD error = ReceiveMessage(pipe, type, payload);
    if (error != ERROR_SUCCESS) {
        LogError(L"ReceiveMessage", error);
        return;
    }

    switch (type) {
    case MessageType::Ping: {
        const PingResult result{kProtocolVersion, GetCurrentProcessId(), 0};
        SendMessage(pipe, MessageType::Pong, &result, sizeof(result));
        break;
    }
    case MessageType::Launch: {
        LaunchResult result{};
        HandleLaunch(pipe, payload, result);
        SendMessage(pipe, MessageType::LaunchResult, &result, sizeof(result));
        break;
    }
    case MessageType::AllowAdd: {
        OperationResult result{};
        HandleAllowAdd(pipe, payload, result);
        SendMessage(pipe, MessageType::AllowResult, &result, sizeof(result));
        break;
    }
    case MessageType::AllowRemove: {
        OperationResult result{};
        HandleAllowRemove(pipe, payload, result);
        SendMessage(pipe, MessageType::AllowResult, &result, sizeof(result));
        break;
    }
    case MessageType::AllowList:
        HandleAllowList(pipe);
        break;
    default:
        LogLine(L"收到未知消息类型 " + std::to_wstring(static_cast<int>(type)));
        break;
    }
}

void ServiceServer::HandleLaunch(HANDLE pipe, const std::vector<std::byte>& payload, LaunchResult& result) {
    result.status = ERROR_SUCCESS;
    result.childPid = 0;
    result.message[0] = L'\0';

    std::wstring executable;
    std::wstring arguments;
    if (!ParseLaunchPayload(payload.data(), payload.size(), executable, arguments)) {
        result.status = ERROR_INVALID_DATA;
        SetResultMessage(result, L"请求格式错误。");
        return;
    }

    if (!ImpersonateNamedPipeClient(pipe)) {
        result.status = GetLastError();
        SetResultMessage(result, L"ImpersonateNamedPipeClient 失败");
        LogError(L"ImpersonateNamedPipeClient", result.status);
        return;
    }
    ImpersonationScope impersonation(true);

    UniqueHandle clientToken;
    std::wstring caller;
    DWORD error = DuplicateClientPrimaryToken(clientToken, caller);
    if (error != ERROR_SUCCESS) {
        result.status = error;
        SetResultMessage(result, L"无法获取调用者令牌" + FormatWin32Error(error));
        LogError(L"DuplicateClientPrimaryToken", error);
        return;
    }

    impersonation.Revert();  // drop the client's identity before using our own privileges

    VerifiedImage image;
    std::wstring reason;
    error = ResolveAllowedExecutable(executable, image, reason);
    if (error != ERROR_SUCCESS) {
        result.status = error;
        SetResultMessage(result, reason);
        LogLine(L"拒绝来自 " + caller + L" 的请求 [" + executable + L"]：" + reason);
        return;
    }

    DWORD childPid = 0;
    std::wstring failure;
    error = LaunchWithUIAccess(clientToken.get(), image, arguments, childPid, failure);
    if (error != ERROR_SUCCESS) {
        result.status = error;
        SetResultMessage(result, failure);
        LogLine(L"启动失败 [" + image.path + L"]：" + failure);
        return;
    }

    result.childPid = childPid;
    LogLine(L"已为 " + caller + L" 以 UIAccess 启动 " + image.path + L" (PID " +
            std::to_wstring(childPid) + L")");
}

void ServiceServer::HandleAllowAdd(HANDLE pipe, const std::vector<std::byte>& payload,
                                   OperationResult& result) {
    result.status = ERROR_SUCCESS;
    result.reserved = 0;
    result.message[0] = L'\0';

    if (!ImpersonateNamedPipeClient(pipe)) {
        result.status = GetLastError();
        SetResultMessage(result, L"ImpersonateNamedPipeClient 失败");
        LogError(L"ImpersonateNamedPipeClient", result.status);
        return;
    }
    ImpersonationScope impersonation(true);

    UniqueHandle clientToken;
    std::wstring caller;
    DWORD error = DuplicateClientPrimaryToken(clientToken, caller);
    impersonation.Revert();
    if (error != ERROR_SUCCESS) {
        result.status = error;
        SetResultMessage(result, L"无法获取调用者令牌" + FormatWin32Error(error));
        return;
    }

    if (!IsElevatedAdministratorToken(clientToken.get(), error)) {
        result.status = error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error;
        SetResultMessage(result, L"修改白名单需要以管理员身份运行。");
        LogLine(L"拒绝 " + caller + L" 的白名单注册请求。");
        return;
    }

    std::vector<std::wstring> parts;
    if (!ParseStringPayload(payload.data(), payload.size(), parts) || parts.size() != 2) {
        result.status = ERROR_INVALID_DATA;
        SetResultMessage(result, L"请求格式错误。");
        return;
    }

    std::wstring message;
    error = AddAllowEntry(parts[0], parts[1], message);
    result.status = error;
    SetResultMessage(result, message);

    if (error == ERROR_SUCCESS) {
        LogLine(L"管理员 " + caller + L" 注册了白名单条目：" + parts[0]);
    }
}

void ServiceServer::HandleAllowRemove(HANDLE pipe, const std::vector<std::byte>& payload,
                                      OperationResult& result) {
    result.status = ERROR_SUCCESS;
    result.reserved = 0;
    result.message[0] = L'\0';

    if (!ImpersonateNamedPipeClient(pipe)) {
        result.status = GetLastError();
        SetResultMessage(result, L"ImpersonateNamedPipeClient 失败");
        LogError(L"ImpersonateNamedPipeClient", result.status);
        return;
    }
    ImpersonationScope impersonation(true);

    UniqueHandle clientToken;
    std::wstring caller;
    DWORD error = DuplicateClientPrimaryToken(clientToken, caller);
    impersonation.Revert();
    if (error != ERROR_SUCCESS) {
        result.status = error;
        SetResultMessage(result, L"无法获取调用者令牌" + FormatWin32Error(error));
        return;
    }

    if (!IsElevatedAdministratorToken(clientToken.get(), error)) {
        result.status = error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error;
        SetResultMessage(result, L"修改白名单需要以管理员身份运行。");
        LogLine(L"拒绝 " + caller + L" 的白名单删除请求。");
        return;
    }

    std::vector<std::wstring> parts;
    if (!ParseStringPayload(payload.data(), payload.size(), parts) || parts.size() != 1) {
        result.status = ERROR_INVALID_DATA;
        SetResultMessage(result, L"请求格式错误。");
        return;
    }

    std::wstring message;
    error = RemoveAllowEntry(parts[0], message);
    result.status = error;
    SetResultMessage(result, message);

    if (error == ERROR_SUCCESS) {
        LogLine(L"管理员 " + caller + L" 删除了白名单条目：" + parts[0]);
    }
}

void ServiceServer::HandleAllowList(HANDLE pipe) {
    std::vector<AllowEntryInfo> entries;
    std::wstring message;
    ListAllowEntries(entries, message);

    std::vector<std::byte> payload;
    for (const AllowEntryInfo& entry : entries) {
        const std::vector<std::byte> record =
            MakeStringPayload({entry.name, entry.path, entry.sha256});
        if (payload.size() + record.size() > kMaxPayloadBytes) {
            LogLine(L"白名单条目过多，列表响应已截断。");
            break;
        }
        payload.insert(payload.end(), record.begin(), record.end());
    }

    SendMessage(pipe, MessageType::AllowListResult, payload.data(), static_cast<uint32_t>(payload.size()));
}

} // namespace uiauf

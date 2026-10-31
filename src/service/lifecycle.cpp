#include "service/lifecycle.h"

#include <sddl.h>
#include <wtsapi32.h>

#include <string>
#include <vector>

#include "common/hash.h"
#include "common/protocol.h"
#include "common/win32_helpers.h"
#include "service/allowlist.h"
#include "service/log.h"

namespace uiauf {
namespace {

constexpr wchar_t kSetupKey[] = L"SOFTWARE\\UIA-UF\\Setup";

// 100 ns units, one day.
constexpr ULONGLONG kDay = 24ull * 60ull * 60ull * 10000000ull;
constexpr ULONGLONG kDefaultIdleDays = 15;
constexpr ULONGLONG kDefaultWarnDays = 15;

struct RegKeyCloser {
    void operator()(HKEY key) const noexcept {
        if (key != nullptr) {
            RegCloseKey(key);
        }
    }
};

using UniqueRegKey = std::unique_ptr<HKEY__, RegKeyCloser>;

std::wstring ClientBinaryPath() {
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (length == 0 || length >= std::size(module)) {
        return {};
    }
    return ParentDirectory(std::wstring(module, length)) + L"\\uiauf.exe";
}

bool HashFile(const std::wstring& path, std::wstring& digest) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    return ComputeSha256Hex(file.get(), digest) == ERROR_SUCCESS;
}

bool ReadString(HKEY key, const wchar_t* name, std::wstring& value) {
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0) {
        return false;
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) {
        return false;
    }
    value = buffer.data();
    return !value.empty();
}

bool ReadQword(HKEY key, const wchar_t* name, ULONGLONG& value) {
    DWORD bytes = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_QWORD, nullptr, &value, &bytes) == ERROR_SUCCESS;
}

bool ReadDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD bytes = sizeof(value);
    return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS;
}

// Policy thresholds are overridable through the SYSTEM-only Setup key; tests and
// integrators use that to shorten the 15 day periods.
ULONGLONG ReadDays(HKEY key, const wchar_t* name, ULONGLONG fallback) {
    DWORD value = 0;
    return ReadDword(key, name, value) ? value : fallback;
}

void WriteQword(const wchar_t* name, ULONGLONG value) {
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, kSetupKey, name, REG_QWORD, &value, sizeof(value));
}

void WriteString(const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, kSetupKey, name, REG_SZ, value.c_str(),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

UniqueRegKey OpenSetupKey(REGSAM access) {
    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSetupKey, 0, access | KEY_WOW64_64KEY, &raw) != ERROR_SUCCESS) {
        return {};
    }
    return UniqueRegKey(raw);
}

// The framework is considered unused when no allow list entry points at a file
// that still exists. Only existence is checked here, never the hash: this is a
// hygiene heuristic, not an authorisation decision.
bool IsOrphaned() {
    std::vector<AllowEntryInfo> entries;
    std::wstring message;
    ListAllowEntries(entries, message);

    if (entries.empty()) {
        return true;
    }
    for (const AllowEntryInfo& entry : entries) {
        if (!entry.path.empty() &&
            GetFileAttributesW(entry.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return false;
        }
    }
    return true;
}

DWORD LaunchInSession(DWORD sessionId, const std::wstring& executable, const std::wstring& arguments) {
    HANDLE rawProcessToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawProcessToken)) {
        return GetLastError();
    }
    UniqueHandle processToken(rawProcessToken);
    for (const wchar_t* name : {SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME, SE_INCREASE_QUOTA_NAME}) {
        EnablePrivilege(processToken.get(), name);
    }

    HANDLE rawUserToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &rawUserToken)) {
        return GetLastError();
    }
    UniqueHandle userToken(rawUserToken);

    HANDLE rawPrimary = nullptr;
    if (!DuplicateTokenEx(userToken.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary,
                          &rawPrimary)) {
        return GetLastError();
    }
    UniqueHandle primary(rawPrimary);

    std::wstring commandLine = QuoteArgument(executable);
    if (!arguments.empty()) {
        commandLine.push_back(L' ');
        commandLine += arguments;
    }

    const std::wstring workingDirectory = ParentDirectory(executable);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION child{};
    if (!CreateProcessAsUserW(primary.get(), executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                              CREATE_UNICODE_ENVIRONMENT, nullptr,
                              workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup,
                              &child)) {
        return GetLastError();
    }

    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return ERROR_SUCCESS;
}

// Returns true when the registered client binary still matches the hash that was
// captured at install time.
bool VerifyRegisteredClient(HKEY key, std::wstring& clientPath) {
    std::wstring expected;
    if (!ReadString(key, L"ClientSha256", expected) || !ReadString(key, L"ClientPath", clientPath)) {
        return false;
    }

    std::wstring actual;
    if (!HashFile(clientPath, actual)) {
        return false;
    }
    return EqualsIgnoreCase(actual, expected);
}

bool ShowIdleWarning(DWORD sessionId, const std::wstring& clientPath, const std::wstring& text,
                     ULONGLONG warnDays) {
    const std::wstring title = L"UIA-UF 闲置提醒";
    const std::wstring arguments =
        L"notify --title " + QuoteArgument(title) + L" --message " + QuoteArgument(text);

    const DWORD error = LaunchInSession(sessionId, clientPath, arguments);
    if (error != ERROR_SUCCESS) {
        LogError(L"LaunchInSession(notify)", error);
        return false;
    }

    LogLine(L"已向会话 " + std::to_wstring(sessionId) + L" 发送闲置提醒（宽限期 " +
            std::to_wstring(warnDays) + L" 天）。");
    return true;
}

// Disables the service in the SCM and asks ServiceMain to stop. Reversible:
// `--install` puts the start type back to demand start.
void DisableSelfAndStop(HANDLE stopEvent) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        LogError(L"OpenSCManager (auto disable)", GetLastError());
        return;
    }

    SC_HANDLE service = OpenServiceW(scm, kServiceName, SERVICE_CHANGE_CONFIG);
    if (service == nullptr) {
        LogError(L"OpenService (auto disable)", GetLastError());
        CloseServiceHandle(scm);
        return;
    }

    const BOOL changed = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
                                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    const DWORD error = changed ? ERROR_SUCCESS : GetLastError();

    CloseServiceHandle(service);
    CloseServiceHandle(scm);

    if (error != ERROR_SUCCESS) {
        LogError(L"ChangeServiceConfig(SERVICE_DISABLED)", error);
        return;
    }

    WriteQword(L"AutoDisabledAt", NowFileTime());
    LogLine(L"闲置宽限期已过且仍无可用条目，服务已在 SCM 中被自动禁用（可用 --install 恢复）。");

    if (stopEvent != nullptr) {
        SetEvent(stopEvent);
    }
}

} // namespace

DWORD EnsureSetupRegistration(const wchar_t* setupMode, std::wstring& message) {
    message.clear();

    HKEY rawKey = nullptr;
    const LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSetupKey, 0, nullptr, 0,
                                           KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &rawKey, nullptr);
    if (status != ERROR_SUCCESS) {
        message = L"无法创建安装登记项" + FormatWin32Error(status);
        return status;
    }
    UniqueRegKey key(rawKey);

    if (setupMode != nullptr) {
        const DWORD standalone = (_wcsicmp(setupMode, L"--standalone") == 0) ? 1 : 0;
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, kSetupKey, L"Standalone", REG_DWORD, &standalone,
                        sizeof(standalone));
        LogLine(standalone != 0 ? L"安装模式：独立安装（跳过闲置清理检查）"
                                : L"安装模式：捆绑安装（启用闲置清理检查）");

        // Coming back from an automatic disable is an explicit "I still want
        // this" signal: start a fresh warning cycle instead of disabling again
        // on the next evaluation.
        ULONGLONG autoDisabledAt = 0;
        if (ReadQword(key.get(), L"AutoDisabledAt", autoDisabledAt)) {
            RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kSetupKey, L"WarnedAt");
            RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kSetupKey, L"AutoDisabledAt");
            LogLine(L"已从自动禁用状态恢复，闲置提醒重新计时。");
        }
    }

    ULONGLONG installedAt = 0;
    if (!ReadQword(key.get(), L"InstalledAt", installedAt)) {
        WriteQword(L"InstalledAt", NowFileTime());
    }

    std::wstring registeredHash;
    const bool forced = setupMode != nullptr;
    if (forced || !ReadString(key.get(), L"ClientSha256", registeredHash)) {
        const std::wstring clientPath = ClientBinaryPath();
        std::wstring digest;
        if (clientPath.empty() || !HashFile(clientPath, digest)) {
            message = L"无法登记客户端哈希（找不到 uiauf.exe）";
            LogLine(message);
            return ERROR_FILE_NOT_FOUND;
        }

        WriteString(L"ClientPath", clientPath);
        WriteString(L"ClientSha256", digest);
        // Legacy values from earlier builds.
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kSetupKey, L"ClientLastWrite");

        LogLine(L"已登记客户端：" + clientPath);
        LogLine(L"  哈希 = " + digest);
    }

    return ERROR_SUCCESS;
}

void EvaluateIdlePolicy(DWORD sessionId, HANDLE stopEvent) {
    UniqueRegKey key = OpenSetupKey(KEY_ALL_ACCESS);
    if (!key) {
        return;
    }

    DWORD standalone = 0;
    if (ReadDword(key.get(), L"Standalone", standalone) && standalone != 0) {
        return;
    }

    const ULONGLONG idleDays = ReadDays(key.get(), L"IdleDays", kDefaultIdleDays);
    const ULONGLONG warnDays = ReadDays(key.get(), L"WarnDays", kDefaultWarnDays);

    const ULONGLONG now = NowFileTime();
    ULONGLONG warnedAt = 0;
    const bool warned = ReadQword(key.get(), L"WarnedAt", warnedAt);

    if (!IsOrphaned()) {
        // Back in use: forget the warning so a future idle episode can warn again.
        if (warned) {
            RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kSetupKey, L"WarnedAt");
            LogLine(L"检测到可用白名单条目，闲置提醒状态已清除。");
        }
        return;
    }

    if (warned) {
        if (now >= warnedAt && now - warnedAt < warnDays * kDay) {
            return;
        }
        // Grace period elapsed: stop serving, but stay installed and reversible.
        DisableSelfAndStop(stopEvent);
        return;
    }

    // Without an interactive session the warning cannot be shown, and the grace
    // clock must not start silently behind the user's back.
    if (sessionId == 0 || sessionId == 0xFFFFFFFF) {
        return;
    }

    // The idle clock starts at the last allow list modification, falling back to
    // the installation time on a setup whose allow list was never touched.
    ULONGLONG changedAt = 0;
    if (!ReadAllowListChangedAt(changedAt) && !ReadQword(key.get(), L"InstalledAt", changedAt)) {
        return;
    }
    if (now < changedAt || now - changedAt < idleDays * kDay) {
        return;
    }

    std::wstring clientPath;
    if (!VerifyRegisteredClient(key.get(), clientPath)) {
        LogLine(L"客户端哈希与登记值不一致，跳过闲置提醒。");
        return;
    }

    const std::wstring text = L"UIA-UF 已超过 " + std::to_wstring(idleDays) +
                              L" 天没有可用的白名单条目（为空，或条目指向的程序已不存在）。请在 " +
                              std::to_wstring(warnDays) +
                              L" 天内添加条目或卸载框架，否则服务将被自动禁用。";

    if (ShowIdleWarning(sessionId, clientPath, text, warnDays)) {
        WriteQword(L"WarnedAt", now);
    }
}

} // namespace uiauf

#include "service/launcher.h"

#include <userenv.h>

#include <iterator>
#include <vector>

#include "service/log.h"

namespace uiauf {

DWORD DuplicateClientPrimaryToken(UniqueHandle& token, std::wstring& callerName) {
    callerName.clear();

    HANDLE rawThreadToken = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_DUPLICATE | TOKEN_QUERY, TRUE, &rawThreadToken)) {
        return GetLastError();
    }
    UniqueHandle threadToken(rawThreadToken);

    DWORD size = 0;
    GetTokenInformation(threadToken.get(), TokenUser, nullptr, 0, &size);
    if (size > 0) {
        std::vector<BYTE> buffer(size);
        if (GetTokenInformation(threadToken.get(), TokenUser, buffer.data(), size, &size)) {
            const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
            wchar_t account[256]{};
            wchar_t domain[256]{};
            DWORD accountLength = static_cast<DWORD>(std::size(account));
            DWORD domainLength = static_cast<DWORD>(std::size(domain));
            SID_NAME_USE use = SidTypeUnknown;
            if (LookupAccountSidW(nullptr, user->User.Sid, account, &accountLength, domain, &domainLength,
                                  &use)) {
                callerName = domainLength > 0 ? std::wstring(domain) + L"\\" + account : std::wstring(account);
            }
        }
    }

    HANDLE primary = nullptr;
    if (!DuplicateTokenEx(threadToken.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary,
                          &primary)) {
        return GetLastError();
    }

    token.reset(primary);
    return ERROR_SUCCESS;
}

namespace {

// The image section already exists once CreateProcess returns, so checking the
// identity before ResumeThread guarantees that user code never runs from a
// swapped image, even if a parent directory gets replaced afterwards.
DWORD VerifySuspendedImage(HANDLE process, const ImageIdentity& expected, std::wstring& errorText) {
    std::vector<wchar_t> path(32768);
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
        const DWORD error = GetLastError();
        errorText = L"无法读取新进程映像路径" + FormatWin32Error(error);
        return error;
    }

    ImageIdentity actual{};
    if (!IdentityFromPath(std::wstring(path.data(), length), actual)) {
        const DWORD error = GetLastError();
        errorText = L"无法校验新进程映像" + FormatWin32Error(error);
        return error != ERROR_SUCCESS ? error : ERROR_FILE_NOT_FOUND;
    }

    if (!(actual == expected)) {
        errorText = L"目标文件在验证后、启动前被替换，已终止进程。";
        return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}

} // namespace

DWORD LaunchWithUIAccess(HANDLE primaryToken, const VerifiedImage& image, const std::wstring& arguments,
                         DWORD& childPid, std::wstring& errorText) {
    childPid = 0;
    errorText.clear();

    // image.file keeps the bytes pinned (share mode denies write/delete) for
    // the whole duration of this call.
    const std::wstring& executable = image.path;

    HANDLE rawProcessToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawProcessToken)) {
        return GetLastError();
    }
    UniqueHandle processToken(rawProcessToken);

    // SeTcbPrivilege        : required to modify TokenUIAccess.
    // SeAssignPrimaryToken  : required by CreateProcessAsUser.
    // SeIncreaseQuota       : required by CreateProcessAsUser.
    for (const wchar_t* name : {SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME, SE_INCREASE_QUOTA_NAME}) {
        const DWORD error = EnablePrivilege(processToken.get(), name);
        if (error != ERROR_SUCCESS) {
            errorText = std::wstring(L"无法启用特权 ") + name + FormatWin32Error(error);
            return error;
        }
    }

    BOOL isUIAccess = TRUE;
    if (!SetTokenInformation(primaryToken, TokenUIAccess, &isUIAccess, sizeof(isUIAccess))) {
        const DWORD error = GetLastError();
        errorText = L"设置 TokenUIAccess 失败" + FormatWin32Error(error);
        return error;
    }

    std::wstring commandLine = QuoteArgument(executable);
    if (!arguments.empty()) {
        commandLine.push_back(L' ');
        commandLine += arguments;
    }

    const std::wstring workingDirectory = ParentDirectory(executable);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    // Without this the child would inherit SYSTEM's environment block.
    void* environment = nullptr;
    const bool hasEnvironment = CreateEnvironmentBlock(&environment, primaryToken, FALSE) != FALSE;

    PROCESS_INFORMATION child{};
    const BOOL created = CreateProcessAsUserW(primaryToken, executable.c_str(), commandLine.data(), nullptr,
                                              nullptr, FALSE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                              environment,
                                              workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                              &startup, &child);

    if (hasEnvironment) {
        DestroyEnvironmentBlock(environment);
    }

    if (!created) {
        const DWORD error = GetLastError();
        errorText = L"CreateProcessAsUser 失败" + FormatWin32Error(error);
        return error;
    }

    DWORD error = VerifySuspendedImage(child.hProcess, image.identity, errorText);
    if (error != ERROR_SUCCESS) {
        TerminateProcess(child.hProcess, static_cast<UINT>(-1));
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return error;
    }

    if (ResumeThread(child.hThread) == static_cast<DWORD>(-1)) {
        error = GetLastError();
        TerminateProcess(child.hProcess, static_cast<UINT>(-1));
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        errorText = L"ResumeThread 失败" + FormatWin32Error(error);
        return error;
    }

    childPid = child.dwProcessId;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return ERROR_SUCCESS;
}

} // namespace uiauf

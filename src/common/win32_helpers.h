#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uiauf {

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

inline UniqueHandle WrapHandle(HANDLE handle) {
    return UniqueHandle(handle);
}

inline std::wstring FormatWin32Error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        wchar_t fallback[64];
        swprintf_s(fallback, L"Win32 error %lu", code);
        return fallback;
    }

    std::wstring text(buffer, length);
    LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    wchar_t prefix[32];
    swprintf_s(prefix, L" (%lu)", code);
    return text + prefix;
}

// Reads the mandatory integrity level RID of a token (0x2000 medium, 0x3000 high,
// 0x4000 system). TokenElevation is unreliable on duplicated/impersonation
// tokens, so elevation decisions use the integrity level instead.
inline bool IntegrityLevelRid(HANDLE token, DWORD& rid) {
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    if (size == 0) {
        return false;
    }

    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
        return false;
    }

    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    const UCHAR subAuthorityCount = *GetSidSubAuthorityCount(label->Label.Sid);
    if (subAuthorityCount == 0) {
        return false;
    }
    rid = *GetSidSubAuthority(label->Label.Sid, subAuthorityCount - 1);
    return true;
}

inline bool IsHighIntegrityToken(HANDLE token) {
    DWORD rid = 0;
    return IntegrityLevelRid(token, rid) && rid >= SECURITY_MANDATORY_HIGH_RID;
}

inline bool IsElevated() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return false;
    }
    UniqueHandle token(rawToken);
    return IsHighIntegrityToken(token.get());
}

inline ULONGLONG NowFileTime() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

inline bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {    if (left.size() != right.size()) {
        return false;
    }
    return _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

inline std::wstring ParentDirectory(std::wstring_view path) {
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring_view::npos) {
        return {};
    }
    if (separator == 2 && path.size() > 2 && path[1] == L':') {
        return std::wstring(path.substr(0, 3));
    }
    return std::wstring(path.substr(0, separator));
}

// Quotes a single argument following the CommandLineToArgvW rules, so that it
// survives the trip through CreateProcessAsUser.
inline std::wstring QuoteArgument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');

    auto cursor = argument.begin();
    for (;;) {
        size_t backslashes = 0;
        while (cursor != argument.end() && *cursor == L'\\') {
            ++cursor;
            ++backslashes;
        }

        if (cursor == argument.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }

        if (*cursor == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(*cursor);
        }
        ++cursor;
    }

    quoted.push_back(L'"');
    return quoted;
}

// Enables a privilege on a token. Returns ERROR_SUCCESS when the privilege
// ended up enabled (AdjustTokenPrivileges reports success even when it did not).
inline DWORD EnablePrivilege(HANDLE token, const wchar_t* privilegeName) {
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        return GetLastError();
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr)) {
        return GetLastError();
    }
    return GetLastError();
}

// Strips the \\?\ / \\?\UNC\ prefix that GetFinalPathNameByHandleW returns.
inline std::wstring StripNtPrefix(std::wstring path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        return L"\\\\" + path.substr(8);
    }
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path.substr(4);
    }
    return path;
}

inline std::wstring FinalPathOf(HANDLE file) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return StripNtPrefix(std::wstring(buffer.data(), length));
}

} // namespace uiauf

#include "service/allowlist.h"


#include <iterator>
#include <string>
#include <vector>

#include "common/hash.h"
#include "common/protocol.h"
#include "common/win32_helpers.h"
#include "service/log.h"

namespace uiauf {
namespace {

struct AllowEntry {
    std::wstring path;
    std::wstring sha256;
};

struct RegKeyCloser {
    void operator()(HKEY key) const noexcept {
        if (key != nullptr) {
            RegCloseKey(key);
        }
    }
};

using UniqueRegKey = std::unique_ptr<HKEY__, RegKeyCloser>;

constexpr DWORD kMaxPathChars = 32768;

// Lexically normalises a path. Does not touch the file system.
DWORD NormalizePath(const std::wstring& input, std::wstring& output) {
    std::vector<wchar_t> buffer(kMaxPathChars);
    const DWORD length = GetFullPathNameW(input.c_str(), kMaxPathChars, buffer.data(), nullptr);
    if (length == 0) {
        return GetLastError();
    }
    if (length >= kMaxPathChars) {
        return ERROR_BUFFER_OVERFLOW;
    }

    output.assign(buffer.data(), length);
    const size_t rootLength = (output.size() >= 3 && output[1] == L':') ? 3 : 1;
    while (output.size() > rootLength && (output.back() == L'\\' || output.back() == L'/')) {
        output.pop_back();
    }
    return ERROR_SUCCESS;
}

// Strips the \\?\ prefix that GetFinalPathNameByHandleW returns.
std::wstring StripNtPrefix(std::wstring path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        return L"\\\\" + path.substr(8);
    }
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        return path.substr(4);
    }
    return path;
}


std::vector<AllowEntry> LoadAllowList() {
    std::vector<AllowEntry> entries;

    HKEY rawKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryAllowKey, 0, KEY_READ | KEY_WOW64_64KEY, &rawKey) !=
        ERROR_SUCCESS) {
        return entries;
    }
    UniqueRegKey key(rawKey);

    for (DWORD index = 0;; ++index) {
        wchar_t subKeyName[256]{};
        DWORD nameLength = static_cast<DWORD>(std::size(subKeyName));
        const LSTATUS status =
            RegEnumKeyExW(static_cast<HKEY>(key.get()), index, subKeyName, &nameLength, nullptr, nullptr,
                          nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            break;
        }

        HKEY rawSubKey = nullptr;
        if (RegOpenKeyExW(static_cast<HKEY>(key.get()), subKeyName, 0, KEY_READ | KEY_WOW64_64KEY,
                          &rawSubKey) != ERROR_SUCCESS) {
            continue;
        }
        UniqueRegKey subKey(rawSubKey);

        auto readString = [&subKey](const wchar_t* valueName) -> std::wstring {
            DWORD type = 0;
            DWORD bytes = 0;
            if (RegGetValueW(static_cast<HKEY>(subKey.get()), nullptr, valueName,
                             RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, nullptr, &bytes) != ERROR_SUCCESS ||
                bytes == 0) {
                return {};
            }

            std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
            if (RegGetValueW(static_cast<HKEY>(subKey.get()), nullptr, valueName,
                             RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, buffer.data(), &bytes) !=
                ERROR_SUCCESS) {
                return {};
            }
            return std::wstring(buffer.data());
        };

        AllowEntry entry;
        entry.path = readString(L"Path");
        entry.sha256 = readString(L"Sha256");
        if (!entry.path.empty()) {
            entries.push_back(std::move(entry));
        }
    }

    return entries;
}

bool IsSha256(const std::wstring& text) {
    if (text.size() != 64) {
        return false;
    }
    for (const wchar_t character : text) {
        const bool hex = (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f') ||
                         (character >= L'A' && character <= L'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

} // namespace

DWORD ResolveAllowedExecutable(const std::wstring& requested, std::wstring& resolvedPath,
                               std::wstring& reason) {
    resolvedPath.clear();
    reason.clear();

    if (requested.empty()) {
        reason = L"请求为空。";
        return ERROR_INVALID_PARAMETER;
    }

    std::wstring absolutePath;
    DWORD error = NormalizePath(requested, absolutePath);
    if (error != ERROR_SUCCESS) {
        reason = L"无法规范化路径" + FormatWin32Error(error);
        return error;
    }

    UniqueHandle file(CreateFileW(absolutePath.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        reason = L"无法打开目标可执行文件" + FormatWin32Error(error);
        return error;
    }

    wchar_t finalPathBuffer[kMaxPathChars]{};
    const DWORD finalLength =
        GetFinalPathNameByHandleW(file.get(), finalPathBuffer, kMaxPathChars, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (finalLength == 0 || finalLength >= kMaxPathChars) {
        error = finalLength == 0 ? GetLastError() : ERROR_BUFFER_OVERFLOW;
        reason = L"无法解析最终路径" + FormatWin32Error(error);
        return error;
    }
    resolvedPath = StripNtPrefix(std::wstring(finalPathBuffer, finalLength));

    std::wstring digest;
    error = ComputeSha256Hex(file.get(), digest);
    if (error != ERROR_SUCCESS) {
        reason = L"无法计算文件哈希" + FormatWin32Error(error);
        return error;
    }

    // Reject directories masquerading as executables.
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) {
        error = GetLastError();
        reason = L"无法读取文件信息" + FormatWin32Error(error);
        return error;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        reason = L"目标是目录，不是可执行文件。";
        return ERROR_ACCESS_DENIED;
    }

    const std::vector<AllowEntry> entries = LoadAllowList();
    if (entries.empty()) {
        reason = L"白名单为空（HKLM\\SOFTWARE\\UIA-UF\\Allow）。请先由管理员添加条目。";
        return ERROR_ACCESS_DENIED;
    }

    for (const AllowEntry& entry : entries) {
        std::wstring entryPath;
        if (NormalizePath(entry.path, entryPath) != ERROR_SUCCESS) {
            continue;
        }
        if (!EqualsIgnoreCase(entryPath, resolvedPath)) {
            continue;
        }
        if (IsSha256(entry.sha256) && !EqualsIgnoreCase(entry.sha256, digest)) {
            reason = L"白名单条目存在，但文件哈希不匹配（可能已更新或已被替换）。";
            LogLine(L"allowlist hash mismatch for " + resolvedPath);
            return ERROR_ACCESS_DENIED;
        }
        return ERROR_SUCCESS;
    }

    reason = L"目标不在白名单中：" + resolvedPath;
    return ERROR_ACCESS_DENIED;
}

} // namespace uiauf
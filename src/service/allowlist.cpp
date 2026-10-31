#include "service/allowlist.h"

#include <aclapi.h>
#include <sddl.h>

#include <iterator>
#include <string>
#include <vector>

#include "common/hash.h"
#include "common/protocol.h"
#include "common/win32_helpers.h"
#include "service/log.h"

namespace uiauf {
namespace {

// SYSTEM full control, administrators read-only. The DACL is protected (no
// inheritance) and the key owner is SYSTEM, so an administrator cannot rewrite
// the DACL without taking ownership first.
constexpr wchar_t kAllowListSddl[] = L"D:P(A;CI;KA;;;SY)(A;CI;KR;;;BA)";

struct RegKeyCloser {
    void operator()(HKEY key) const noexcept {
        if (key != nullptr) {
            RegCloseKey(key);
        }
    }
};

using UniqueRegKey = std::unique_ptr<HKEY__, RegKeyCloser>;

struct AllowEntry {
    std::wstring name;
    std::wstring path;
    std::wstring sha256;
};

constexpr DWORD kMaxPathChars = 32768;
constexpr wchar_t kLastChangedValue[] = L"LastChanged";

void WriteAllowListChangedAt(ULONGLONG fileTime) {
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, kRegistryAllowKey, kLastChangedValue, REG_QWORD, &fileTime,
                    sizeof(fileTime));
}

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

std::wstring BaseNameOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        name.resize(dot);
    }
    return name;
}

bool IsSha256(const std::wstring& text) {
    return IsSha256Hex(text);
}

std::wstring ReadStringValue(HKEY key, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        bytes == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
    if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type, buffer.data(),
                     &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return std::wstring(buffer.data());
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
        wchar_t name[256]{};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LSTATUS status =
            RegEnumKeyExW(key.get(), index, name, &nameLength, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            break;
        }

        HKEY rawSubKey = nullptr;
        if (RegOpenKeyExW(key.get(), name, 0, KEY_READ | KEY_WOW64_64KEY, &rawSubKey) != ERROR_SUCCESS) {
            continue;
        }
        UniqueRegKey subKey(rawSubKey);

        AllowEntry entry;
        entry.name = name;
        entry.path = ReadStringValue(subKey.get(), L"Path");
        entry.sha256 = ReadStringValue(subKey.get(), L"Sha256");
        if (!entry.path.empty()) {
            entries.push_back(std::move(entry));
        }
    }

    return entries;
}

// An executable opened for validation. The handle denies writes and deletes, so
// the bytes cannot be swapped while it is alive.
struct OpenedImage {
    UniqueHandle file;
    std::wstring path;
    std::wstring digest;
    ImageIdentity identity;
};

DWORD OpenImage(const std::wstring& requested, OpenedImage& opened, std::wstring& reason) {
    opened = OpenedImage{};
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

    UniqueHandle file(CreateFileW(absolutePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        reason = L"无法打开目标可执行文件" + FormatWin32Error(error);
        return error;
    }

    const std::wstring finalPath = FinalPathOf(file.get());
    if (finalPath.empty()) {
        error = GetLastError();
        reason = L"无法解析最终路径" + FormatWin32Error(error);
        return error;
    }

    std::wstring digest;
    error = ComputeSha256Hex(file.get(), digest);
    if (error != ERROR_SUCCESS) {
        reason = L"无法计算文件哈希" + FormatWin32Error(error);
        return error;
    }

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

    ImageIdentity identity{};
    if (!IdentityFromHandle(file.get(), identity)) {
        error = GetLastError();
        reason = L"无法读取文件标识" + FormatWin32Error(error);
        return error;
    }

    opened.file = std::move(file);
    opened.path = finalPath;
    opened.digest = std::move(digest);
    opened.identity = identity;
    return ERROR_SUCCESS;
}

} // namespace

bool ReadAllowListChangedAt(ULONGLONG& fileTime) {
    DWORD bytes = sizeof(fileTime);
    return RegGetValueW(HKEY_LOCAL_MACHINE, kRegistryAllowKey, kLastChangedValue, RRF_RT_REG_QWORD, nullptr,
                        &fileTime, &bytes) == ERROR_SUCCESS;
}

DWORD EnsureAllowListKey(std::wstring& message) {
    message.clear();

    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken)) {
        const DWORD error = GetLastError();
        message = L"无法打开进程令牌" + FormatWin32Error(error);
        return error;
    }
    UniqueHandle processToken(rawToken);

    // Needed to set the owner of an existing key to SYSTEM.
    EnablePrivilege(processToken.get(), SE_RESTORE_NAME);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kAllowListSddl, SDDL_REVISION_1, &descriptor,
                                                             nullptr)) {
        const DWORD error = GetLastError();
        message = L"构建安全描述符失败" + FormatWin32Error(error);
        return error;
    }

    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID systemSid = nullptr;
    if (!AllocateAndInitializeSid(&authority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &systemSid)) {
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        message = L"构建 SYSTEM SID 失败" + FormatWin32Error(error);
        return error;
    }

    PACL dacl = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl, &daclDefaulted);

    // Absolute-format descriptor carrying both the SYSTEM owner and the
    // protected DACL; RegSetKeySecurity applies both in one call.
    SECURITY_DESCRIPTOR absolute{};
    InitializeSecurityDescriptor(&absolute, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorOwner(&absolute, systemSid, FALSE);
    SetSecurityDescriptorDacl(&absolute, TRUE, dacl, FALSE);
    SetSecurityDescriptorControl(&absolute, SE_DACL_PROTECTED, SE_DACL_PROTECTED);

    DWORD result = ERROR_SUCCESS;
    for (const wchar_t* path : {L"SOFTWARE\\UIA-UF", kRegistryAllowKey}) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;

        HKEY rawKey = nullptr;
        const LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                               WRITE_OWNER | WRITE_DAC | KEY_READ, &attributes, &rawKey, nullptr);
        if (status != ERROR_SUCCESS) {
            result = status;
            message = L"无法打开注册表项 " + std::wstring(path) + FormatWin32Error(status);
            break;
        }
        UniqueRegKey key(rawKey);

        // SetSecurityInfo rejects registry objects, so ownership and the
        // protected DACL both go through RegSetKeySecurity.
        const DWORD stepError = RegSetKeySecurity(
            key.get(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &absolute);
        if (stepError != ERROR_SUCCESS) {
            result = stepError;
            message = L"无法收紧注册表项 " + std::wstring(path) + FormatWin32Error(stepError);
            break;
        }

        DWORD bytes = 0;
        RegGetKeySecurity(key.get(), DACL_SECURITY_INFORMATION, nullptr, &bytes);
        if (bytes == 0) {
            result = ERROR_INVALID_DATA;
            message = L"无法读取注册表项 DACL " + std::wstring(path);
            break;
        }

        std::vector<BYTE> buffer(bytes);
        const LSTATUS readStatus = RegGetKeySecurity(
            key.get(), DACL_SECURITY_INFORMATION, reinterpret_cast<PSECURITY_DESCRIPTOR>(buffer.data()),
            &bytes);
        if (readStatus != ERROR_SUCCESS) {
            result = readStatus;
            message = L"无法读取注册表项 DACL " + std::wstring(path) + FormatWin32Error(readStatus);
            break;
        }

        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(reinterpret_cast<PSECURITY_DESCRIPTOR>(buffer.data()), &control,
                                          &revision) ||
            (control & SE_DACL_PROTECTED) == 0) {
            result = ERROR_INVALID_STATE;
            message = L"注册表项 DACL 未能标记为受保护：" + std::wstring(path);
            break;
        }
    }

    FreeSid(systemSid);
    LocalFree(descriptor);

    if (result == ERROR_SUCCESS) {
        // Seed the idle-cleanup clock the first time the key is created; do not
        // refresh it on later starts, otherwise every boot would reset it.
        ULONGLONG changedAt = 0;
        if (!ReadAllowListChangedAt(changedAt)) {
            WriteAllowListChangedAt(NowFileTime());
        }
        LogLine(L"白名单注册表项已限制为仅 SYSTEM 可写。");
    } else {
        LogLine(L"收紧白名单注册表项失败：" + message);
    }
    return result;
}

DWORD ResolveAllowedExecutable(const std::wstring& requested, VerifiedImage& image, std::wstring& reason) {
    image = VerifiedImage{};
    reason.clear();

    OpenedImage opened;
    DWORD error = OpenImage(requested, opened, reason);
    if (error != ERROR_SUCCESS) {
        return error;
    }

    const std::vector<AllowEntry> entries = LoadAllowList();
    if (entries.empty()) {
        reason = L"白名单为空（HKLM\\SOFTWARE\\UIA-UF\\Allow）。请先由管理员注册。";
        return ERROR_ACCESS_DENIED;
    }

    for (const AllowEntry& entry : entries) {
        std::wstring entryPath;
        if (NormalizePath(entry.path, entryPath) != ERROR_SUCCESS) {
            continue;
        }
        if (!EqualsIgnoreCase(entryPath, opened.path)) {
            continue;
        }

        // Hash pinning is mandatory: an entry without a usable Sha256 authorises
        // nothing.
        if (!IsSha256(entry.sha256)) {
            reason = L"条目 [" + entry.name + L"] 缺少有效的 Sha256，已按未授权处理。";
            LogLine(L"allowlist entry without valid hash: " + entry.name);
            return ERROR_ACCESS_DENIED;
        }
        if (!EqualsIgnoreCase(entry.sha256, opened.digest)) {
            reason = L"条目 [" + entry.name + L"] 的哈希与文件不匹配（可能已更新或被替换）。";
            LogLine(L"allowlist hash mismatch for " + opened.path);
            return ERROR_ACCESS_DENIED;
        }

        image.file = std::move(opened.file);
        image.path = opened.path;
        image.identity = opened.identity;
        return ERROR_SUCCESS;
    }

    reason = L"目标不在白名单中：" + opened.path;
    return ERROR_ACCESS_DENIED;
}

DWORD AddAllowEntry(const std::wstring& requested, const std::wstring& expectedSha256, std::wstring& message) {
    message.clear();

    if (!IsSha256(expectedSha256)) {
        message = L"注册时必须提供 64 位十六进制的 SHA-256。";
        return ERROR_INVALID_PARAMETER;
    }

    std::wstring reason;
    OpenedImage opened;
    DWORD error = OpenImage(requested, opened, reason);
    if (error != ERROR_SUCCESS) {
        message = reason;
        return error;
    }

    if (!EqualsIgnoreCase(opened.digest, expectedSha256)) {
        message = L"调用者提供的预期哈希与文件实际内容不一致，未写入。\n  预期 = " + expectedSha256 +
                  L"\n  实际 = " + opened.digest;
        LogLine(L"allowlist add rejected, hash mismatch for " + opened.path);
        return ERROR_INVALID_DATA;
    }

    std::wstring keyMessage;
    error = EnsureAllowListKey(keyMessage);
    if (error != ERROR_SUCCESS) {
        message = L"无法准备白名单注册表项：" + keyMessage;
        return error;
    }

    const std::wstring name = BaseNameOf(opened.path);
    const std::wstring subKey = std::wstring(kRegistryAllowKey) + L"\\" + name;

    LSTATUS status = RegSetKeyValueW(HKEY_LOCAL_MACHINE, subKey.c_str(), L"Path", REG_SZ, opened.path.c_str(),
                                     static_cast<DWORD>((opened.path.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        message = L"写入 Path 失败" + FormatWin32Error(status);
        return status;
    }

    status = RegSetKeyValueW(HKEY_LOCAL_MACHINE, subKey.c_str(), L"Sha256", REG_SZ, opened.digest.c_str(),
                             static_cast<DWORD>((opened.digest.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        message = L"写入 Sha256 失败" + FormatWin32Error(status);
        return status;
    }

    message = L"已注册 [" + name + L"]\n  Path   = " + opened.path + L"\n  Sha256 = " + opened.digest;
    WriteAllowListChangedAt(NowFileTime());
    LogLine(L"allowlist entry added: " + name + L" -> " + opened.path);
    return ERROR_SUCCESS;
}

DWORD RemoveAllowEntry(const std::wstring& name, std::wstring& message) {
    message.clear();

    if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos) {
        message = L"条目名称非法。";
        return ERROR_INVALID_PARAMETER;
    }

    const std::wstring subKey = std::wstring(kRegistryAllowKey) + L"\\" + name;
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, subKey.c_str());
    if (status != ERROR_SUCCESS) {
        message = L"删除失败" + FormatWin32Error(status);
        return status;
    }

    message = L"已删除 [" + name + L"]";
    WriteAllowListChangedAt(NowFileTime());
    LogLine(L"allowlist entry removed: " + name);
    return ERROR_SUCCESS;
}

DWORD ListAllowEntries(std::vector<AllowEntryInfo>& entries, std::wstring& message) {
    entries.clear();
    message.clear();

    for (const AllowEntry& entry : LoadAllowList()) {
        AllowEntryInfo info;
        info.name = entry.name;
        info.path = entry.path;
        info.sha256 = entry.sha256;
        entries.push_back(std::move(info));
    }
    return ERROR_SUCCESS;
}

} // namespace uiauf

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Wire protocol shared by UIA-UF.Service.exe and uiauf.exe.
//
// Transport: named pipe, byte mode, message framed by MessageHeader.
// All strings on the wire are UTF-16LE.

namespace uiauf {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\UIA-UF.v1";

inline constexpr wchar_t kServiceName[] = L"UIA-UF";
inline constexpr wchar_t kServiceDisplayName[] = L"UIA-UF Service";
inline constexpr wchar_t kServiceDescription[] =
    L"Launches allow-listed applications with the UIAccess token flag set.";

// HKLM is writable by administrators only: that is the trust anchor of the
// whole framework. See docs in README.md.
inline constexpr wchar_t kRegistryAllowKey[] = L"SOFTWARE\\UIA-UF\\Allow";

inline constexpr uint32_t kProtocolMagic = 0x46414955u; // 'U','I','A','F'
inline constexpr uint16_t kProtocolVersion = 3;
inline constexpr uint32_t kMaxPayloadBytes = 64u * 1024u;

// Allow list administration lives in a key whose DACL only grants write access
// to SYSTEM, so it has to go through the service.
enum class MessageType : uint16_t {
    Ping = 1,
    Pong = 2,
    Launch = 3,
    LaunchResult = 4,
    AllowAdd = 5,
    AllowRemove = 6,
    AllowList = 7,
    AllowResult = 8,
    AllowListResult = 9,
};

#pragma warning(push)
#pragma warning(disable : 4200 4324)
#pragma pack(push, 1)

struct MessageHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payloadSize;
};

struct PingResult {
    uint32_t protocolVersion;
    uint32_t servicePid;
    uint32_t reserved;
};

// status == 0 means success, otherwise it is the Win32 error code that made
// the service give up. message carries a human readable explanation.
struct LaunchResult {
    uint32_t status;
    uint32_t childPid;
    wchar_t message[256];
};

struct OperationResult {
    uint32_t status;
    uint32_t reserved;
    wchar_t message[256];
};

#pragma pack(pop)
#pragma warning(pop)

// Launch payload: executable path, command line arguments and working directory,
// each a NUL terminated UTF-16 string.
inline std::vector<std::byte> MakeLaunchPayload(std::wstring_view exe, std::wstring_view args,
                                                std::wstring_view workingDirectory) {
    const size_t chars = exe.size() + 1 + args.size() + 1 + workingDirectory.size() + 1;
    std::vector<std::byte> payload(chars * sizeof(wchar_t));
    wchar_t* cursor = reinterpret_cast<wchar_t*>(payload.data());

    auto append = [&cursor](std::wstring_view text) {
        if (!text.empty()) {
            std::memcpy(cursor, text.data(), text.size() * sizeof(wchar_t));
        }
        cursor += text.size();
        *cursor++ = L'\0';
    };

    append(exe);
    append(args);
    append(workingDirectory);
    return payload;
}

inline bool ParseLaunchPayload(const std::byte* data, size_t size, std::wstring& exe, std::wstring& args,
                               std::wstring& workingDirectory) {
    if (size < 3 * sizeof(wchar_t) || size % sizeof(wchar_t) != 0) {
        return false;
    }

    const wchar_t* text = reinterpret_cast<const wchar_t*>(data);
    const size_t count = size / sizeof(wchar_t);
    if (text[count - 1] != L'\0') {
        return false;
    }

    std::vector<std::wstring> parts;
    size_t start = 0;
    for (size_t i = 0; i < count; ++i) {
        if (text[i] == L'\0') {
            parts.emplace_back(text + start, i - start);
            start = i + 1;
        }
    }
    if (parts.size() != 3) {
        return false;
    }

    exe = std::move(parts[0]);
    args = std::move(parts[1]);
    workingDirectory = std::move(parts[2]);
    return true;
}

// Generic NUL separated UTF-16 payload, used by the allow list messages.
inline std::vector<std::byte> MakeStringPayload(std::initializer_list<std::wstring_view> parts) {
    size_t chars = 0;
    for (const auto part : parts) {
        chars += part.size() + 1;
    }

    std::vector<std::byte> payload(chars * sizeof(wchar_t));
    wchar_t* cursor = reinterpret_cast<wchar_t*>(payload.data());
    for (const auto part : parts) {
        if (!part.empty()) {
            std::memcpy(cursor, part.data(), part.size() * sizeof(wchar_t));
        }
        cursor += part.size();
        *cursor++ = L'\0';
    }
    return payload;
}

inline bool ParseStringPayload(const std::byte* data, size_t size, std::vector<std::wstring>& parts) {
    parts.clear();
    if (size == 0) {
        return true;
    }
    if (size % sizeof(wchar_t) != 0) {
        return false;
    }

    const wchar_t* text = reinterpret_cast<const wchar_t*>(data);
    const size_t count = size / sizeof(wchar_t);
    if (text[count - 1] != L'\0') {
        return false;
    }

    size_t start = 0;
    for (size_t i = 0; i < count; ++i) {
        if (text[i] == L'\0') {
            parts.emplace_back(text + start, i - start);
            start = i + 1;
        }
    }
    return true;
}

} // namespace uiauf

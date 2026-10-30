#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>

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

inline bool IsElevated() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        return false;
    }
    UniqueHandle token(rawToken);

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &returned);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

inline bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
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

} // namespace uiauf

#include "service/log.h"

#include <string>

#include "common/win32_helpers.h"

namespace uiauf {
namespace {

HANDLE OpenLogFile() {
    wchar_t programData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return INVALID_HANDLE_VALUE;
    }

    const std::wstring directory = std::wstring(programData) + L"\\UIA-UF";
    CreateDirectoryW(directory.c_str(), nullptr);

    const std::wstring file = directory + L"\\service.log";
    return CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

void LogLine(std::wstring_view text) {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t stamp[64]{};
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute, now.wSecond);

    const std::wstring line = std::wstring(stamp) + std::wstring(text) + L"\r\n";
    OutputDebugStringW(line.c_str());

    const HANDLE file = OpenLogFile();
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
        const wchar_t bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    }

    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

void LogError(std::wstring_view what, DWORD error) {
    LogLine(std::wstring(what) + L" failed:" + FormatWin32Error(error));
}

} // namespace uiauf

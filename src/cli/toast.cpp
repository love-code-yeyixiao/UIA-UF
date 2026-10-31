#include "cli/toast.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace uiauf {
namespace {

constexpr wchar_t kWindowClass[] = L"UIA-UF-NotifyWindow";
constexpr UINT kTrayIconId = 1;
constexpr UINT kCallbackMessage = WM_APP + 1;
constexpr ULONGLONG kDisplayMilliseconds = 6000;

LRESULT CALLBACK NotifyWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kCallbackMessage) {
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void PumpMessages(ULONGLONG durationMs) {
    const ULONGLONG deadline = GetTickCount64() + durationMs;
    MSG message{};
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(50);
    }
}

} // namespace

bool ShowToast(const std::wstring& title, const std::wstring& message) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = NotifyWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    HWND window = CreateWindowExW(0, kWindowClass, L"UIA-UF", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  windowClass.hInstance, nullptr);
    if (window == nullptr) {
        return false;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    icon.uCallbackMessage = kCallbackMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    wcsncpy_s(icon.szTip, _countof(icon.szTip), L"UIA-UF", _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &icon)) {
        DestroyWindow(window);
        return false;
    }
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);

    icon.uFlags = NIF_INFO;
    icon.dwInfoFlags = NIIF_WARNING;
    icon.uTimeout = 10000;
    wcsncpy_s(icon.szInfoTitle, _countof(icon.szInfoTitle), title.c_str(), _TRUNCATE);
    wcsncpy_s(icon.szInfo, _countof(icon.szInfo), message.c_str(), _TRUNCATE);

    const bool shown = Shell_NotifyIconW(NIM_MODIFY, &icon) != FALSE;
    if (shown) {
        PumpMessages(kDisplayMilliseconds);
    }

    Shell_NotifyIconW(NIM_DELETE, &icon);
    DestroyWindow(window);
    return shown;
}

} // namespace uiauf

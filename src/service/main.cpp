#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <string_view>

#include "common/protocol.h"
#include "common/win32_helpers.h"
#include "service/service.h"

namespace {

void EnableUnicodeConsole() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
}

int PrintUsage() {
    std::fwprintf(stderr,
                  L"UIA-UF.Service.exe -- UIAccess 服务端\n"
                  L"\n"
                  L"用法:\n"
                  L"  UIA-UF.Service.exe --install     安装并启动服务（需要管理员权限）\n"
                  L"      --StandaloneSetup              独立安装：跳过闲置自动清理检查\n"
                  L"  UIA-UF.Service.exe --uninstall   停止并删除服务（需要管理员权限）\n"
                  L"  UIA-UF.Service.exe --start       启动已安装的服务\n"
                  L"  UIA-UF.Service.exe --stop        停止服务\n"
                  L"  UIA-UF.Service.exe --console     在前台运行服务端，便于调试\n"
                  L"\n"
                  L"不带参数时由服务控制管理器启动。\n");
    return 2;
}

void PrintResult(const wchar_t* action, DWORD error) {
    if (error == ERROR_SUCCESS) {
        std::wprintf(L"%ls: 成功\n", action);
        return;
    }

    const std::wstring text = uiauf::FormatWin32Error(error);
    if (error == ERROR_ACCESS_DENIED) {
        std::wprintf(L"%ls: 失败 %ls\n提示：请以管理员权限运行该命令。\n", action, text.c_str());
    } else {
        std::wprintf(L"%ls: 失败 %ls\n", action, text.c_str());
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    EnableUnicodeConsole();

    if (argc < 2) {
        // Started by the SCM.
        const SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(uiauf::kServiceName), uiauf::ServiceMain},
            {nullptr, nullptr},
        };

        if (!StartServiceCtrlDispatcherW(table)) {
            const std::wstring text = uiauf::FormatWin32Error(GetLastError());
            std::fwprintf(stderr, L"StartServiceCtrlDispatcher 失败: %ls\n", text.c_str());
            return 1;
        }
        return 0;
    }

    const std::wstring_view mode = argv[1];

    if (mode == L"--install") {
        bool standalone = false;
        for (int index = 2; index < argc; ++index) {
            if (_wcsicmp(argv[index], L"--StandaloneSetup") == 0) {
                standalone = true;
            }
        }

        DWORD error = uiauf::InstallService();
        PrintResult(L"安装服务", error);
        if (error != ERROR_SUCCESS) {
            return 1;
        }

        // Restart so the installation mode is applied even if an old instance is running.
        uiauf::StopServiceIfRunning();
        error = uiauf::StartServiceIfNeeded(standalone ? L"--standalone" : L"--bundled");
        PrintResult(L"启动服务", error);
        return error == ERROR_SUCCESS ? 0 : 1;
    }
    if (mode == L"--uninstall") {
        const DWORD error = uiauf::UninstallService();
        PrintResult(L"卸载服务", error);
        return error == ERROR_SUCCESS ? 0 : 1;
    }
    if (mode == L"--start") {
        const DWORD error = uiauf::StartServiceIfNeeded();
        PrintResult(L"启动服务", error);
        return error == ERROR_SUCCESS ? 0 : 1;
    }
    if (mode == L"--stop") {
        const DWORD error = uiauf::StopServiceIfRunning();
        PrintResult(L"停止服务", error);
        return error == ERROR_SUCCESS ? 0 : 1;
    }
    if (mode == L"--console") {
        return uiauf::RunInConsole();
    }

    return PrintUsage();
}

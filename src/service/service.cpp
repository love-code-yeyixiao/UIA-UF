#include "service/service.h"

#include <iterator>
#include <string>

#include "common/protocol.h"
#include "common/win32_helpers.h"
#include "service/log.h"
#include "service/server.h"

namespace uiauf {
namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
DWORD g_checkpoint = 0;
ServiceServer* g_server = nullptr;

void ReportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    g_status.dwCheckPoint =
        (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? ++g_checkpoint : 0;

    if (g_statusHandle != nullptr) {
        SetServiceStatus(g_statusHandle, &g_status);
    }
}

DWORD WINAPI ControlHandler(DWORD control, DWORD eventType, LPVOID eventData, LPVOID context) {
    UNREFERENCED_PARAMETER(eventType);
    UNREFERENCED_PARAMETER(eventData);
    UNREFERENCED_PARAMETER(context);

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (g_server != nullptr) {
            g_server->RequestStop();
        }
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        ReportStatus(g_status.dwCurrentState);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

BOOL WINAPI ConsoleControlHandler(DWORD controlType) {
    if (controlType == CTRL_C_EVENT || controlType == CTRL_BREAK_EVENT || controlType == CTRL_CLOSE_EVENT) {
        if (g_server != nullptr) {
            g_server->RequestStop();
        }
        return TRUE;
    }
    return FALSE;
}

struct ScHandleCloser {
    void operator()(SC_HANDLE handle) const noexcept {
        if (handle != nullptr) {
            CloseServiceHandle(handle);
        }
    }
};

using UniqueScHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ScHandleCloser>;

std::wstring ServiceImagePath() {
    wchar_t module[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (length == 0 || length >= std::size(module)) {
        return {};
    }
    return L"\"" + std::wstring(module, length) + L"\"";
}

} // namespace

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
    if (g_statusHandle == nullptr) {
        LogError(L"RegisterServiceCtrlHandlerEx", GetLastError());
        return;
    }

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 5000);
    LogLine(L"服务正在启动。");

    ServiceServer server;
    g_server = &server;
    if (!server.Start()) {
        g_server = nullptr;
        ReportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }

    ReportStatus(SERVICE_RUNNING);

    WaitForSingleObject(server.StopEvent(), INFINITE);

    ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
    server.Wait();
    g_server = nullptr;

    LogLine(L"服务已停止。");
    ReportStatus(SERVICE_STOPPED);
}

DWORD InstallService() {
    const std::wstring imagePath = ServiceImagePath();
    if (imagePath.empty()) {
        return GetLastError();
    }

    UniqueScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT));
    if (!scm) {
        return GetLastError();
    }

    UniqueScHandle service(CreateServiceW(scm.get(), kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
                                          SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                                          imagePath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!service) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_EXISTS) {
            return error;
        }
        // Already installed: refresh the binary path so the service follows the build output.
        service.reset(OpenServiceW(scm.get(), kServiceName, SERVICE_ALL_ACCESS));
        if (!service) {
            return GetLastError();
        }
        if (!ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                  imagePath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr,
                                  nullptr)) {
            return GetLastError();
        }
    }

    SERVICE_DESCRIPTIONW description{const_cast<LPWSTR>(kServiceDescription)};
    ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &description);

    return ERROR_SUCCESS;
}

DWORD UninstallService() {
    UniqueScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        return GetLastError();
    }

    UniqueScHandle service(OpenServiceW(scm.get(), kServiceName,
                                        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE | SERVICE_CHANGE_CONFIG));
    if (!service) {
        return GetLastError();
    }

    SERVICE_STATUS status{};
    if (ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (!QueryServiceStatus(service.get(), &status) || status.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
            Sleep(100);
        }
    }

    if (!DeleteService(service.get())) {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

DWORD StartServiceIfNeeded() {
    UniqueScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        return GetLastError();
    }

    UniqueScHandle service(OpenServiceW(scm.get(), kServiceName, SERVICE_START | SERVICE_QUERY_STATUS));
    if (!service) {
        return GetLastError();
    }

    if (!StartServiceW(service.get(), 0, nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) {
            return error;
        }
    }
    return ERROR_SUCCESS;
}

DWORD StopServiceIfRunning() {
    UniqueScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        return GetLastError();
    }

    UniqueScHandle service(OpenServiceW(scm.get(), kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!service) {
        return GetLastError();
    }

    SERVICE_STATUS status{};
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
            return error;
        }
    }
    return ERROR_SUCCESS;
}

int RunInConsole() {
    wprintf(L"以控制台模式运行服务端（Ctrl+C 退出）。\n");
    if (!IsElevated()) {
        wprintf(L"警告：当前进程未提升，无法获得 SeTcbPrivilege，启动请求会失败。\n");
    } else {
        wprintf(L"提示：SeTcbPrivilege 通常只授予 LocalSystem，普通管理员身份仍可能失败。\n");
    }

    ServiceServer server;
    g_server = &server;
    if (!server.Start()) {
        g_server = nullptr;
        wprintf(L"服务端启动失败，详见 %%ProgramData%%\\UIA-UF\\service.log。\n");
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    WaitForSingleObject(server.StopEvent(), INFINITE);
    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);

    server.Wait();
    g_server = nullptr;
    return 0;
}

} // namespace uiauf

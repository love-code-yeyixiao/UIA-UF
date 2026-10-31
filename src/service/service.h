#pragma once

#include <windows.h>

namespace uiauf {

// Entry point handed to StartServiceCtrlDispatcherW.
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);

// SCM helpers, used by UIA-UF.Service.exe command line switches.
DWORD InstallService();
DWORD UninstallService();
DWORD StartServiceIfNeeded(const wchar_t* setupMode = nullptr);
DWORD StopServiceIfRunning();

// Runs the server in the foreground, for debugging. Requires an elevated
// (ideally SYSTEM) context to actually be able to launch with UIAccess.
int RunInConsole();

} // namespace uiauf

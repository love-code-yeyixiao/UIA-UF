#pragma once

#include <string>

namespace uiauf {

// Shows a notification in the current interactive session and blocks until the
// shell has had a chance to render it (about six seconds).
//
// Implemented with Shell_NotifyIcon: an unpackaged Win32 process has no
// AppUserModelID, so the WinRT toast API is not available without registering a
// shortcut first. Windows 10/11 route these balloons through the same
// notification platform, so the user sees a toast in the Action Center.
bool ShowToast(const std::wstring& title, const std::wstring& message);

} // namespace uiauf

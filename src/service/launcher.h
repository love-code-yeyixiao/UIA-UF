#pragma once

#include <windows.h>

#include <string>

#include "common/win32_helpers.h"

namespace uiauf {

// Duplicates the impersonation token of the calling thread (the named pipe
// client) into a primary token owned by this function. Must be called with
// ImpersonateNamedPipeClient active.
DWORD DuplicateClientPrimaryToken(UniqueHandle& token, std::wstring& callerName);

// Enables a privilege on the given token. Returns ERROR_SUCCESS when the
// privilege ended up enabled.
DWORD EnablePrivilege(HANDLE token, const wchar_t* privilegeName);

// Stamps TokenUIAccess onto `primaryToken` and starts `executable` on the
// interactive desktop of that token's session.
DWORD LaunchWithUIAccess(HANDLE primaryToken, const std::wstring& executable, const std::wstring& arguments,
                         DWORD& childPid, std::wstring& errorText);

} // namespace uiauf

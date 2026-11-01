#pragma once

#include <windows.h>

#include <string>

#include "common/verified_image.h"
#include "common/win32_helpers.h"

namespace uiauf {

// Duplicates the impersonation token of the calling thread (the named pipe
// client) into a primary token owned by this function. Must be called with
// ImpersonateNamedPipeClient active.
DWORD DuplicateClientPrimaryToken(UniqueHandle& token, std::wstring& callerName);

// Stamps TokenUIAccess onto `primaryToken` and starts `image` on the
// interactive desktop of that token's session. The process is created
// suspended, its mapped image is compared against `image.identity`, and only
// then resumed. `image.file` must stay open for the duration of this call.
// An empty `workingDirectory` falls back to the executable's own directory.
DWORD LaunchWithUIAccess(HANDLE primaryToken, const VerifiedImage& image, const std::wstring& arguments,
                         const std::wstring& workingDirectory, DWORD& childPid, std::wstring& errorText);

} // namespace uiauf

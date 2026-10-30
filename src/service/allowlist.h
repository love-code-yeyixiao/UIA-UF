#pragma once

#include <windows.h>

#include <string>

namespace uiauf {

// Resolves `requested` to its final path and verifies it against the
// administrator-controlled allow list in HKLM\SOFTWARE\UIA-UF\Allow.
//
// Returns ERROR_SUCCESS when the executable may be launched with UIAccess.
// ERROR_ACCESS_DENIED means "not allow-listed"; any other code is a real
// failure (missing file, unreadable file, ...). `reason` always receives a
// human readable explanation.
DWORD ResolveAllowedExecutable(const std::wstring& requested, std::wstring& resolvedPath,
                               std::wstring& reason);

} // namespace uiauf

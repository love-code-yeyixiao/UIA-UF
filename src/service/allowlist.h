#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "common/verified_image.h"

namespace uiauf {

struct AllowEntryInfo {
    std::wstring name;
    std::wstring path;
    std::wstring sha256;
};

// Resolves `requested` to its final path and verifies it against the
// administrator-controlled allow list in HKLM\SOFTWARE\UIA-UF\Allow.
//
// On success `image` holds the still-open file handle (share mode denies writes
// and deletes), the final path and the file identity; keep it alive until
// CreateProcessAsUser has returned so nothing can be swapped in between.
//
// Returns ERROR_SUCCESS when the executable may be launched with UIAccess.
// ERROR_ACCESS_DENIED means "not allow-listed"; any other code is a real
// failure (missing file, unreadable file, ...). `reason` always receives a
// human readable explanation.
DWORD ResolveAllowedExecutable(const std::wstring& requested, VerifiedImage& image, std::wstring& reason);

// Creates HKLM\SOFTWARE\UIA-UF[\Allow] if needed and rewrites the security
// descriptor so that only LocalSystem can write to it. Called when the service
// starts; requires a LocalSystem token.
DWORD EnsureAllowListKey(std::wstring& message);

// The three functions below run inside the service (LocalSystem): the key is no
// longer writable by administrators. `expectedSha256` must match what the
// service computes for the file, otherwise nothing is written.
DWORD AddAllowEntry(const std::wstring& requested, const std::wstring& expectedSha256, std::wstring& message);
DWORD RemoveAllowEntry(const std::wstring& name, std::wstring& message);
DWORD ListAllowEntries(std::vector<AllowEntryInfo>& entries, std::wstring& message);

} // namespace uiauf

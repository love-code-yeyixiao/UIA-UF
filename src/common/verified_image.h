#pragma once

#include <windows.h>

#include <string>

#include "common/win32_helpers.h"

namespace uiauf {

// Identity of a file on a volume: (volume serial, file index). Stable across
// renames and hard links, and impossible to fake for a different file.
struct ImageIdentity {
    DWORD volumeSerial = 0;
    DWORD fileIndexHigh = 0;
    DWORD fileIndexLow = 0;

    bool operator==(const ImageIdentity&) const = default;
};

inline bool IdentityFromHandle(HANDLE file, ImageIdentity& identity) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) {
        return false;
    }
    identity.volumeSerial = info.dwVolumeSerialNumber;
    identity.fileIndexHigh = info.nFileIndexHigh;
    identity.fileIndexLow = info.nFileIndexLow;
    return true;
}

inline bool IdentityFromPath(const std::wstring& path, ImageIdentity& identity) {
    UniqueHandle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    return IdentityFromHandle(file.get(), identity);
}

// An executable that passed allow list validation. `file` is opened without
// FILE_SHARE_WRITE / FILE_SHARE_DELETE, so the bytes cannot be rewritten or
// swapped by path while this object is alive.
struct VerifiedImage {
    UniqueHandle file;
    std::wstring path;
    ImageIdentity identity;
};

} // namespace uiauf

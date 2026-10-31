#pragma once

#include <windows.h>

#include <bcrypt.h>

#include <string>
#include <vector>

namespace uiauf {

// Hashes the whole file with SHA-256 and returns the digest as lowercase hex.
// The file pointer is rewound first, so a handle opened for raw reading is fine.
inline DWORD ComputeSha256Hex(HANDLE file, std::wstring& digest) {
    digest.clear();

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> hashObject;
    std::vector<unsigned char> hashValue;
    DWORD status = ERROR_SUCCESS;

    do {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            status = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }

        DWORD objectLength = 0;
        DWORD hashLength = 0;
        DWORD returned = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                              sizeof(objectLength), &returned, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
                              sizeof(hashLength), &returned, 0) < 0) {
            status = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }

        hashObject.resize(objectLength);
        hashValue.resize(hashLength);

        if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0) < 0) {
            status = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }

        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
            status = GetLastError();
            break;
        }

        std::vector<unsigned char> buffer(64 * 1024);
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                status = GetLastError();
                break;
            }
            if (read == 0) {
                break;
            }
            if (BCryptHashData(hash, buffer.data(), read, 0) < 0) {
                status = ERROR_GEN_FAILURE;
                break;
            }
        }
        if (status != ERROR_SUCCESS) {
            break;
        }
        if (BCryptFinishHash(hash, hashValue.data(), hashLength, 0) < 0) {
            status = ERROR_GEN_FAILURE;
            break;
        }

        static constexpr wchar_t kDigits[] = L"0123456789abcdef";
        digest.reserve(hashLength * 2);
        for (DWORD i = 0; i < hashLength; ++i) {
            digest.push_back(kDigits[hashValue[i] >> 4]);
            digest.push_back(kDigits[hashValue[i] & 0x0F]);
        }
    } while (false);

    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return status;
}

// Returns true when the string is exactly 64 hexadecimal characters, i.e. a
// SHA-256 digest in the form the wire protocol and registry use.
inline bool IsSha256Hex(std::wstring_view text) {
    if (text.size() != 64) {
        return false;
    }
    for (const wchar_t character : text) {
        const bool hex = (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f') ||
                         (character >= L'A' && character <= L'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

} // namespace uiauf

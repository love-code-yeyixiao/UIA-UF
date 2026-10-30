#pragma once

#include <windows.h>

#include <cstddef>
#include <vector>

#include "common/protocol.h"

namespace uiauf {

namespace detail {

inline DWORD OverlappedIo(HANDLE pipe, void* buffer, DWORD size, bool isWrite, DWORD timeoutMs) {
    auto* cursor = static_cast<BYTE*>(buffer);
    DWORD remaining = size;

    while (remaining > 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr) {
            return GetLastError();
        }

        DWORD transferred = 0;
        const BOOL started = isWrite
                                 ? WriteFile(pipe, cursor, remaining, &transferred, &overlapped)
                                 : ReadFile(pipe, cursor, remaining, &transferred, &overlapped);
        DWORD error = started ? ERROR_SUCCESS : GetLastError();

        if (!started && error == ERROR_IO_PENDING) {
            const DWORD wait = WaitForSingleObject(overlapped.hEvent, timeoutMs);
            if (wait == WAIT_TIMEOUT) {
                CancelIoEx(pipe, &overlapped);
                CloseHandle(overlapped.hEvent);
                return ERROR_TIMEOUT;
            }
            if (wait != WAIT_OBJECT_0) {
                const DWORD waitError = (wait == WAIT_FAILED) ? GetLastError() : ERROR_GEN_FAILURE;
                CloseHandle(overlapped.hEvent);
                return waitError;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
                error = GetLastError();
                CloseHandle(overlapped.hEvent);
                return error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE;
            }
            error = ERROR_SUCCESS;
        }

        CloseHandle(overlapped.hEvent);

        if (error != ERROR_SUCCESS) {
            return error;
        }
        if (transferred == 0) {
            return ERROR_HANDLE_EOF;
        }

        cursor += transferred;
        remaining -= transferred;
    }

    return ERROR_SUCCESS;
}

} // namespace detail

inline constexpr DWORD kDefaultIoTimeoutMs = 10'000;

inline DWORD ReadAll(HANDLE pipe, void* buffer, DWORD size, DWORD timeoutMs = kDefaultIoTimeoutMs) {
    return detail::OverlappedIo(pipe, buffer, size, false, timeoutMs);
}

inline DWORD WriteAll(HANDLE pipe, const void* buffer, DWORD size, DWORD timeoutMs = kDefaultIoTimeoutMs) {
    return detail::OverlappedIo(pipe, const_cast<void*>(buffer), size, true, timeoutMs);
}

inline DWORD SendMessage(HANDLE pipe, MessageType type, const void* payload, uint32_t payloadSize) {
    if (payloadSize > kMaxPayloadBytes) {
        return ERROR_INVALID_PARAMETER;
    }

    const MessageHeader header{
        kProtocolMagic, kProtocolVersion, static_cast<uint16_t>(type), payloadSize};
    DWORD error = WriteAll(pipe, &header, sizeof(header));
    if (error == ERROR_SUCCESS && payloadSize > 0) {
        error = WriteAll(pipe, payload, payloadSize);
    }
    return error;
}

inline DWORD ReceiveMessage(HANDLE pipe, MessageType& type, std::vector<std::byte>& payload) {
    MessageHeader header{};
    DWORD error = ReadAll(pipe, &header, sizeof(header));
    if (error != ERROR_SUCCESS) {
        return error;
    }
    if (header.magic != kProtocolMagic) {
        return ERROR_INVALID_DATA;
    }
    if (header.version != kProtocolVersion) {
        return ERROR_REVISION_MISMATCH;
    }
    if (header.payloadSize > kMaxPayloadBytes) {
        return ERROR_INVALID_DATA;
    }

    payload.assign(header.payloadSize, std::byte{});
    if (header.payloadSize > 0) {
        error = ReadAll(pipe, payload.data(), header.payloadSize);
        if (error != ERROR_SUCCESS) {
            return error;
        }
    }

    type = static_cast<MessageType>(header.type);
    return ERROR_SUCCESS;
}

} // namespace uiauf

// Original Cell Framework implementation; see LICENSE.
#pragma once

namespace cell::files::detail {

// File-system filters can briefly hold a conflicting handle after publication.
// Retry sharing errors only, leaving deliberate access and existence errors intact.
template <class Operation>
DWORD retryWindowsFileSharing(Operation operation) {
    for (DWORD delay = 2; ; delay *= 2) {
        const DWORD error = operation();
        if ((error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) || delay > 32)
            return error;
        Sleep(delay);
    }
}

inline HANDLE openSharedWindowsFile(const wchar_t* path, DWORD access, DWORD flags) {
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto error = retryWindowsFileSharing([&] {
        handle = CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, flags, nullptr);
        return handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    });
    if (handle == INVALID_HANDLE_VALUE)
        SetLastError(error);
    return handle;
}

}

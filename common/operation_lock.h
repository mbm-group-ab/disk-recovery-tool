#pragma once
// One writer per recovery output (DR4/DR6). Every tool that writes recovery data takes this
// lock first: the imager on <dest>.lock, the carver and the exFAT parser on
// <output_dir>\.recovery.lock. The lock file is held open with no sharing and is deleted when
// the handle closes, so a crashed or killed run never leaves a stale lock behind (Windows
// closes the handle with the process). A second run on the same output gets
// ERROR_SHARING_VIOLATION and must stop with kExitBusy before writing anything.
#include <windows.h>

#include <cstdio>
#include <filesystem>

namespace recovery {

constexpr int kExitBusy = 9;  // nothing done: another run is using the same output

class OperationLock {
public:
    OperationLock() = default;
    ~OperationLock() { Release(); }
    OperationLock(const OperationLock&) = delete;
    OperationLock& operator=(const OperationLock&) = delete;

    // Returns false when another process holds the lock (Busy() is then true) or when the lock
    // file cannot be created (LastError() says why).
    bool Acquire(const std::filesystem::path& lock_path, const char* tool) {
        Release();
        handle_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            last_error_ = GetLastError();
            return false;
        }
        char text[128];
        const int length = snprintf(text, sizeof(text), "%s pid %lu\r\n", tool,
                                    static_cast<unsigned long>(GetCurrentProcessId()));
        DWORD written = 0;
        if (length > 0) WriteFile(handle_, text, static_cast<DWORD>(length), &written, nullptr);
        return true;
    }

    void Release() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

    bool Busy() const {
        return last_error_ == ERROR_SHARING_VIOLATION || last_error_ == ERROR_LOCK_VIOLATION;
    }
    DWORD LastError() const { return last_error_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    DWORD last_error_ = ERROR_SUCCESS;
};

}  // namespace recovery

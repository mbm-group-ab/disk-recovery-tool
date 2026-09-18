#pragma once

// libstdc++ on MinGW does not recognise UNC root names ("\\server\share"), so
// std::filesystem::absolute() silently re-roots such paths onto the current
// drive and create_directories() walks the wrong components. Everything that
// may touch a network share goes through these Win32-backed helpers instead.

#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace recovery {

// Absolute, normalised form of `path` as Windows itself resolves it.
inline std::filesystem::path FullPathWin32(const std::filesystem::path& path) {
    const std::wstring native = path.native();
    if (native.empty()) return path;
    DWORD needed = GetFullPathNameW(native.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;
    std::wstring full(needed, L'\0');
    const DWORD written = GetFullPathNameW(native.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed) return path;
    full.resize(written);
    return std::filesystem::path(full);
}

// std::filesystem::path::is_absolute() is false for UNC paths under libstdc++;
// this answers the question the way Win32 does.
inline bool IsAbsoluteWin32(const std::filesystem::path& path) {
    const std::wstring& native = path.native();
    if (native.size() >= 2 && (native[0] == L'\\' || native[0] == L'/') &&
        (native[1] == L'\\' || native[1] == L'/')) return true;
    return native.size() >= 3 && native[1] == L':' && (native[2] == L'\\' || native[2] == L'/');
}

// Length of the non-creatable prefix: "C:\" or "\\server\share\".
inline size_t RootPrefixLength(const std::wstring& full) {
    if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\') {
        size_t position = 2;
        for (int component = 0; component < 2; ++component) {
            position = full.find(L'\\', position);
            if (position == std::wstring::npos) return full.size();
            ++position;
        }
        return position;
    }
    if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\') return 3;
    return 0;
}

// mkdir -p that works for drive-letter and UNC paths alike.
inline bool CreateDirectoriesWin32(const std::filesystem::path& path, std::error_code& ec) {
    ec.clear();
    const std::wstring full = FullPathWin32(path).native();
    if (full.empty()) {
        ec = std::error_code(ERROR_INVALID_NAME, std::system_category());
        return false;
    }
    size_t position = RootPrefixLength(full);
    while (position <= full.size()) {
        size_t next = full.find(L'\\', position);
        if (next == std::wstring::npos) next = full.size();
        if (next > position) {
            const std::wstring prefix = full.substr(0, next);
            if (!CreateDirectoryW(prefix.c_str(), nullptr)) {
                const DWORD error = GetLastError();
                if (error != ERROR_ALREADY_EXISTS) {
                    ec = std::error_code(static_cast<int>(error), std::system_category());
                    return false;
                }
            }
        }
        if (next == full.size()) break;
        position = next + 1;
    }
    const DWORD attributes = GetFileAttributesW(full.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        ec = std::error_code(ERROR_PATH_NOT_FOUND, std::system_category());
        return false;
    }
    return true;
}

}  // namespace recovery

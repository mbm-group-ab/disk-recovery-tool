#include "random_access_reader.h"
#include "sha256.h"
#include "win32_paths.h"

#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <malloc.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace recovery {

std::filesystem::path Utf8Path(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), wide.data(), length) != length) return {};
    return std::filesystem::path(wide);
}

RandomAccessReader::~RandomAccessReader() { Close(); }

bool RandomAccessReader::Open(const std::filesystem::path& path, uint64_t device_size_override) {
#ifdef RECOVERY_REGULAR_FILES_ONLY
    (void)device_size_override;
#endif
    if (!Close()) return false;
    if (path.extension() == L".rci") return OpenChunkIndex(path);
    const std::wstring native = path.native();
    is_device_ = native.rfind(L"\\\\.\\", 0) == 0;
    // FILE_SHARE_DELETE is meaningless for a raw device handle and some USB
    // bridge/class drivers reject the open outright (ERROR_INVALID_FUNCTION)
    // when it's requested, so only ask for it on regular files.
    const DWORD share_mode = is_device_ ? (FILE_SHARE_READ | FILE_SHARE_WRITE)
                                         : (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    // Some USB mass-storage bridges reject buffered raw-sector reads with
    // ERROR_NOT_READY even while the mounted filesystem remains accessible.
    // Unbuffered I/O with explicitly aligned offsets, lengths and memory is
    // the most interoperable path for disk and volume device handles.
    const DWORD open_flags = is_device_ ? (FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS)
                                        : FILE_ATTRIBUTE_NORMAL;
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, share_mode,
                          nullptr, OPEN_EXISTING, open_flags, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        last_error_ = GetLastError();
        return false;
    }
#ifndef RECOVERY_REGULAR_FILES_ONLY
    if (is_device_ && device_size_override != 0) {
        size_ = device_size_override;
    } else {
      GET_LENGTH_INFORMATION length{};
      DWORD returned = 0;
      if (DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                        &length, sizeof(length), &returned, nullptr)) {
        size_ = static_cast<uint64_t>(length.Length.QuadPart);
      } else if (is_device_) {
        DISK_GEOMETRY_EX geometry{};
        if (DeviceIoControl(handle_, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                            &geometry, sizeof(geometry), &returned, nullptr) &&
            geometry.DiskSize.QuadPart > 0) {
            size_ = static_cast<uint64_t>(geometry.DiskSize.QuadPart);
        } else {
            STORAGE_READ_CAPACITY capacity{};
            capacity.Version = sizeof(capacity);
            capacity.Size = sizeof(capacity);
            if (DeviceIoControl(handle_, IOCTL_STORAGE_READ_CAPACITY, nullptr, 0,
                                &capacity, sizeof(capacity), &returned, nullptr) &&
                capacity.DiskLength.QuadPart > 0) {
                size_ = static_cast<uint64_t>(capacity.DiskLength.QuadPart);
            } else {
                last_error_ = GetLastError();
                Close();
                return false;
            }
        }
      } else
#endif
      {
        LARGE_INTEGER file_size{};
        if (!GetFileSizeEx(handle_, &file_size) || file_size.QuadPart < 0) {
            last_error_ = GetLastError();
            Close();
            return false;
        }
        size_ = static_cast<uint64_t>(file_size.QuadPart);
        file_size_ = size_;
        // A truncated image (e.g. only the real part of a fake-capacity drive
        // was imaged) can be presented at its full logical size; the missing
        // tail reads as zeros, matching what the device itself returned.
        if (device_size_override > size_) size_ = device_size_override;
    }
#ifndef RECOVERY_REGULAR_FILES_ONLY
    }
#endif
    if (is_device_) {
#ifndef RECOVERY_REGULAR_FILES_ONLY
        DISK_GEOMETRY geometry{};
        DWORD geometry_bytes = 0;
        if (DeviceIoControl(handle_, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0,
                            &geometry, sizeof(geometry), &geometry_bytes, nullptr) &&
            geometry.BytesPerSector >= 512 &&
            (geometry.BytesPerSector & (geometry.BytesPerSector - 1)) == 0) {
            device_sector_size_ = geometry.BytesPerSector;
        }
#endif
    }
    last_error_ = ERROR_SUCCESS;
    return true;
}

bool RandomAccessReader::Close() {
    file_size_ = 0;
    bool ok = true;
    DWORD close_error = ERROR_SUCCESS;
    if (handle_ != INVALID_HANDLE_VALUE && !CloseHandle(handle_)) {
        ok = false;
        close_error = GetLastError();
    }
    handle_ = INVALID_HANDLE_VALUE;
    for (auto& segment : segments_) {
        if (segment.handle != INVALID_HANDLE_VALUE && !CloseHandle(segment.handle) && ok) {
            ok = false;
            close_error = GetLastError();
        }
    }
    segments_.clear();
    size_ = 0;
    is_device_ = false;
    chunked_ = false;
    lazy_chunks_ = false;
    device_sector_size_ = 512;
    if (!ok) last_error_ = close_error;
    return ok;
}

bool RandomAccessReader::ReadHandleAt(HANDLE handle, uint64_t offset, void* buffer, size_t size) {
    auto* output = static_cast<uint8_t*>(buffer);
    size_t completed = 0;
    while (completed < size) {
        const DWORD amount = static_cast<DWORD>(std::min<size_t>(
            size - completed, std::numeric_limits<DWORD>::max()));
        const uint64_t position = offset + completed;
        LARGE_INTEGER file_position{};
        file_position.QuadPart = static_cast<LONGLONG>(position);
        if (!SetFilePointerEx(handle, file_position, nullptr, FILE_BEGIN)) {
            last_error_ = GetLastError();
            return false;
        }
        DWORD read = 0;
        const BOOL ok = ReadFile(handle, output + completed, amount, &read, nullptr);
        if (!ok || read != amount) {
            last_error_ = ok ? ERROR_HANDLE_EOF : GetLastError();
            return false;
        }
        completed += read;
    }
    last_error_ = ERROR_SUCCESS;
    return true;
}

bool RandomAccessReader::ReadAt(uint64_t offset, void* buffer, size_t size) {
    if (!IsOpen() || offset > size_ || size > size_ - offset) {
        last_error_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (size == 0) {
        last_error_ = ERROR_SUCCESS;
        return true;
    }
    if (!chunked_) {
        if (is_device_) {
            const uint64_t aligned_start = offset - (offset % device_sector_size_);
            const uint64_t requested_end = offset + size;
            const uint64_t aligned_end = std::min<uint64_t>(size_,
                (requested_end + device_sector_size_ - 1) &
                    ~(static_cast<uint64_t>(device_sector_size_) - 1));
            if (aligned_end < requested_end || aligned_end - aligned_start >
                std::numeric_limits<size_t>::max()) {
                last_error_ = ERROR_INVALID_PARAMETER;
                return false;
            }
            const size_t bounce_size = static_cast<size_t>(aligned_end - aligned_start);
            const size_t memory_alignment = std::max<size_t>(4096, device_sector_size_);
            void* bounce = _aligned_malloc(bounce_size, memory_alignment);
            if (!bounce) {
                last_error_ = ERROR_NOT_ENOUGH_MEMORY;
                return false;
            }
            const bool read_ok = ReadHandleAt(handle_, aligned_start, bounce, bounce_size);
            if (read_ok) {
                memcpy(buffer, static_cast<uint8_t*>(bounce) + (offset - aligned_start), size);
            }
            _aligned_free(bounce);
            if (!read_ok) return false;
            return true;
        }
        if (file_size_ != 0 && offset + size > file_size_) {
            const size_t real = offset < file_size_ ? static_cast<size_t>(file_size_ - offset) : 0;
            if (real != 0 && !ReadHandleAt(handle_, offset, buffer, real)) return false;
            memset(static_cast<uint8_t*>(buffer) + real, 0, size - real);
            last_error_ = ERROR_SUCCESS;
            return true;
        }
        return ReadHandleAt(handle_, offset, buffer, size);
    }

    auto* output = static_cast<uint8_t*>(buffer);
    uint64_t cursor = offset;
    size_t completed = 0;
    while (completed < size) {
        const auto it = std::upper_bound(segments_.begin(), segments_.end(), cursor,
            [](uint64_t value, const Segment& segment) { return value < segment.offset; });
        const bool in_gap = it == segments_.begin() ||
            cursor >= std::prev(it)->offset + std::prev(it)->size;
        if (in_gap) {
            if (!lazy_chunks_) {
                last_error_ = ERROR_READ_FAULT;
                return false;
            }
            // A lazily written image never created a part here: the writer
            // only skips regions whose source bytes were all zero.
            const uint64_t gap_end = it == segments_.end() ? size_ : it->offset;
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(
                size - completed, gap_end - cursor));
            memset(output + completed, 0, amount);
            cursor += amount;
            completed += amount;
            continue;
        }
        const Segment& segment = *std::prev(it);
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(
            size - completed, segment.offset + segment.size - cursor));
        if (!ReadHandleAt(segment.handle, cursor - segment.offset, output + completed, amount))
            return false;
        cursor += amount;
        completed += amount;
    }
    return true;
}

bool RandomAccessReader::OpenChunkIndex(const std::filesystem::path& path) {
    std::ifstream index(path);
    std::string header;
    if (!std::getline(index, header) || header != "#recovery-chunks 1") {
        last_error_ = ERROR_BAD_FORMAT;
        return false;
    }
    std::string tag;
    if (!(index >> tag >> size_) || tag != "total" || size_ == 0) {
        last_error_ = ERROR_BAD_FORMAT;
        return false;
    }
    uint64_t previous_end = 0;
    bool lazy_parts = false;
    while (index >> tag) {
        // Lazy indexes carry their layout and destinations; readers only need
        // to know that gaps between parts are legitimate and read as zeros.
        if (tag == "lazy") {
            int flag = 0;
            if (!(index >> flag) || flag != 1 || !segments_.empty()) {
                last_error_ = ERROR_BAD_FORMAT;
                Close();
                return false;
            }
            lazy_parts = true;
            continue;
        }
        if (tag == "chunk") {
            uint64_t chunk = 0;
            if (!(index >> chunk) || chunk == 0 || !segments_.empty()) {
                last_error_ = ERROR_BAD_FORMAT;
                Close();
                return false;
            }
            continue;
        }
        if (tag == "dest") {
            std::string root;
            uint64_t capacity = 0;
            if (!(index >> std::quoted(root) >> capacity) || !segments_.empty()) {
                last_error_ = ERROR_BAD_FORMAT;
                Close();
                return false;
            }
            continue;
        }
        if (tag != "part") {
            last_error_ = ERROR_BAD_FORMAT;
            Close();
            return false;
        }
        uint64_t offset = 0, size = 0;
        std::string part_name;
        if (!(index >> offset >> size >> std::quoted(part_name)) || size == 0 ||
            offset > size_ || size > size_ - offset || offset < previous_end ||
            (!lazy_parts && offset != previous_end)) {
            last_error_ = ERROR_BAD_FORMAT;
            Close();
            return false;
        }
        std::string remainder, expected_hash;
        std::getline(index, remainder);
        if (!remainder.empty()) {
            std::istringstream attributes(remainder);
            std::string attribute, extra;
            if (attributes >> attribute) {
                if (attribute != "sha256" || !(attributes >> expected_hash) ||
                    expected_hash.size() != 64 || (attributes >> extra)) {
                    last_error_ = ERROR_BAD_FORMAT;
                    Close();
                    return false;
                }
            }
        }
        fs::path part_path = Utf8Path(part_name);
        if (part_path.empty()) {
            last_error_ = ERROR_BAD_FORMAT;
            Close();
            return false;
        }
        if (!IsAbsoluteWin32(part_path)) part_path = path.parent_path() / part_path;
        part_path = FullPathWin32(part_path);
        HANDLE part = CreateFileW(part_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (part == INVALID_HANDLE_VALUE) {
            last_error_ = GetLastError();
            Close();
            return false;
        }
        LARGE_INTEGER actual{};
        if (!GetFileSizeEx(part, &actual) || actual.QuadPart < 0 ||
            static_cast<uint64_t>(actual.QuadPart) < size) {
            last_error_ = ERROR_BAD_LENGTH;
            CloseHandle(part);
            Close();
            return false;
        }
        if (!expected_hash.empty()) {
            const auto actual_hash = Sha256File(part_path);
            if (!actual_hash || *actual_hash != expected_hash) {
                last_error_ = ERROR_CRC;
                CloseHandle(part);
                Close();
                return false;
            }
        }
        segments_.push_back({offset, size, part});
        previous_end = offset + size;
    }
    if (!lazy_parts && (segments_.empty() || previous_end != size_)) {
        last_error_ = ERROR_BAD_FORMAT;
        Close();
        return false;
    }
    lazy_chunks_ = lazy_parts;
    chunked_ = true;
    is_device_ = false;
    last_error_ = ERROR_SUCCESS;
    return true;
}

}  // namespace recovery

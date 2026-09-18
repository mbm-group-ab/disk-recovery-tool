#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace recovery {

class RandomAccessReader {
public:
    RandomAccessReader() = default;
    ~RandomAccessReader();
    RandomAccessReader(const RandomAccessReader&) = delete;
    RandomAccessReader& operator=(const RandomAccessReader&) = delete;

    bool Open(const std::filesystem::path& path, uint64_t device_size_override = 0);
    bool Close();
    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE || chunked_; }
    uint64_t Size() const { return size_; }
    bool IsDevice() const { return is_device_; }
    DWORD LastErrorCode() const { return last_error_; }
    bool ReadAt(uint64_t offset, void* buffer, size_t size);

private:
    struct Segment {
        uint64_t offset;
        uint64_t size;
        HANDLE handle;
    };
    bool OpenChunkIndex(const std::filesystem::path& path);
    bool ReadHandleAt(HANDLE handle, uint64_t file_offset, void* buffer, size_t size);

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    uint64_t file_size_ = 0;  // regular file: real length; bytes past it read as zeros
    DWORD last_error_ = ERROR_SUCCESS;
    bool is_device_ = false;
    bool chunked_ = false;
    bool lazy_chunks_ = false;  // gaps between parts read as zeros
    uint32_t device_sector_size_ = 512;
    std::vector<Segment> segments_;
};

}  // namespace recovery

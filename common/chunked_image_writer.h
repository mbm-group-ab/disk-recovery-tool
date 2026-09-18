#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace recovery {

struct ChunkDestinationSpec {
    std::filesystem::path root;
    uint64_t capacity = 0;  // 0 means unlimited
};

// Splits one logical image into fixed-size part files spread over several
// destinations. Two layouts exist:
//
//  * eager (default): every part is created up front, so the aggregate
//    destination capacity must cover the whole image;
//  * lazy: a part file is created only the first time non-zero data lands in
//    it. Regions with no part file read back as zeros. This lets a mostly
//    empty 8 TB source be imaged onto much smaller destinations, including
//    filesystems without sparse-file support. The destinations are persisted
//    in the index so a resumed run can keep creating parts.
class ChunkedImageWriter {
public:
    ChunkedImageWriter() = default;
    ~ChunkedImageWriter();
    ChunkedImageWriter(const ChunkedImageWriter&) = delete;
    ChunkedImageWriter& operator=(const ChunkedImageWriter&) = delete;

    bool Create(const std::filesystem::path& index_path, uint64_t total_size,
                uint64_t chunk_size, const std::vector<ChunkDestinationSpec>& destinations,
                const std::filesystem::path& protected_source = {}, bool lazy_parts = false);
    // `destinations` replaces the persisted list of a lazy index when non-empty.
    bool OpenExisting(const std::filesystem::path& index_path, uint64_t expected_total_size,
                      const std::vector<ChunkDestinationSpec>& destinations = {});
    bool WriteAt(uint64_t offset, const void* data, size_t size);
    bool Flush();
    bool Finalize();
    DWORD LastErrorCode() const { return last_error_; }
    bool LazyParts() const { return lazy_parts_; }
    size_t PartCount() const { return parts_.size(); }

private:
    struct Part {
        uint64_t offset;
        uint64_t size;
        std::filesystem::path path;
        HANDLE handle;
        std::string sha256;
    };

    bool WriteIndex();
    void Close();
    Part* FindPart(uint64_t number);
    Part* CreatePart(uint64_t number);
    bool CreatePartFile(const std::filesystem::path& part_path, uint64_t part_size, HANDLE* handle);
    bool PartPathIsProtected(const std::filesystem::path& part_path) const;

    std::filesystem::path index_path_;
    std::filesystem::path protected_source_;
    uint64_t total_size_ = 0;
    uint64_t chunk_size_ = 0;
    bool lazy_parts_ = false;
    std::vector<Part> parts_;  // sorted by offset; may have gaps in lazy mode
    std::vector<ChunkDestinationSpec> destinations_;
    std::vector<uint64_t> destination_used_;
    DWORD last_error_ = ERROR_SUCCESS;
};

}  // namespace recovery

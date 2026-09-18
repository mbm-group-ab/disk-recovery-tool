#pragma once
// Tracks which byte ranges of the source device have been copied to the
// image, and which were bad and skipped. Format is a simple text mapfile
// (inspired by ddrescue's), so a run can be interrupted and resumed:
//
//   <start_offset> <size> <status>
//
// status: '+' = rescued, '-' = bad sector (skipped), '?' = not yet tried
//
#include <cstdint>
#include <string>
#include <vector>

namespace recovery {

enum class RangeStatus : char {
    NotTried = '?',
    Rescued = '+',
    Bad = '-',
    Deferred = '*',
};

struct Range {
    uint64_t offset;
    uint64_t size;
    RangeStatus status;
};

class RescueMap {
public:
    explicit RescueMap(uint64_t total_size);

    // Load an existing mapfile to resume a previous run. Returns false if
    // the file doesn't exist or doesn't match total_size (caller should
    // then start fresh).
    bool Load(const std::string& path);

    // Writes through a temporary file and keeps the previous valid map as
    // <path>.bak. Returns false without replacing the current map on error.
    bool Save(const std::string& path) const;

    // Bind a map to the source/output pair used by the imager. When set,
    // Load rejects maps created for another command identity.
    void ConfigureIdentity(std::string source, std::string destination,
                           uint32_t sector_size);
    const std::string& GenerationId() const { return generation_id_; }
    bool LoadedFromBackup() const { return loaded_from_backup_; }

    void MarkRescued(uint64_t offset, uint64_t size);
    void MarkBad(uint64_t offset, uint64_t size);
    void MarkDeferred(uint64_t offset, uint64_t size);
    void MarkNotTried(uint64_t offset, uint64_t size);

    // Returns the next contiguous range that still needs work (status
    // NotTried), or size==0 if nothing is left.
    Range NextPending(uint64_t max_chunk, bool reverse = false) const;
    std::vector<Range> RangesWithStatus(RangeStatus status, bool reverse = false) const;

    uint64_t TotalRescued() const;
    uint64_t TotalBad() const;
    uint64_t TotalSize() const { return total_size_; }

    // True only when every byte in [offset, offset + size) is marked rescued.
    // Invalid/out-of-bounds ranges are never considered rescued.
    bool IsFullyRescued(uint64_t offset, uint64_t size) const;
    uint64_t UnrescuedBytes(uint64_t offset, uint64_t size) const;

private:
    void SetRange(uint64_t offset, uint64_t size, RangeStatus status);

    uint64_t total_size_;
    std::string source_identity_;
    std::string destination_identity_;
    uint32_t sector_size_ = 0;
    std::string generation_id_;
    bool loaded_from_backup_ = false;
    // Sorted, non-overlapping, contiguous coverage of [0, total_size_).
    std::vector<Range> ranges_;
};

}  // namespace recovery

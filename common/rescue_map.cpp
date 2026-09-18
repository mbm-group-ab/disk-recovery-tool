#include "rescue_map.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace recovery {

RescueMap::RescueMap(uint64_t total_size) : total_size_(total_size) {
    ranges_.push_back({0, total_size, RangeStatus::NotTried});
}

namespace {

struct MapMetadata {
    std::string generation;
    std::string source;
    std::string destination;
    uint32_t sector_size = 0;
};

bool LoadValidated(const std::string& path, uint64_t expected_total,
                   const MapMetadata& expected, std::vector<Range>& loaded,
                   MapMetadata& metadata) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    uint64_t declared_total = 0;
    std::string line;
    if (!std::getline(in, line)) return false;
    {
        std::istringstream iss(line);
        std::string tag;
        iss >> tag >> declared_total;
        if (tag != "#total" || declared_total != expected_total) return false;
    }

    uint64_t expected_offset = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            std::istringstream header(line);
            std::string tag;
            header >> tag;
            if (tag == "#generation") header >> metadata.generation;
            else if (tag == "#source") header >> std::quoted(metadata.source);
            else if (tag == "#destination") header >> std::quoted(metadata.destination);
            else if (tag == "#sector") header >> metadata.sector_size;
            continue;
        }
        std::istringstream iss(line);
        uint64_t offset, size;
        char status_char;
        iss >> offset >> size >> status_char;
        std::string trailing;
        if (!iss || (iss >> trailing) || size == 0 || offset != expected_offset ||
            offset > expected_total || size > expected_total - offset ||
            (status_char != '+' && status_char != '-' && status_char != '?' && status_char != '*')) {
            return false;
        }
        loaded.push_back({offset, size, static_cast<RangeStatus>(status_char)});
        expected_offset += size;
    }
    if (loaded.empty() || expected_offset != expected_total) return false;
    if (!expected.source.empty() &&
        (metadata.generation.empty() || metadata.source != expected.source ||
         metadata.destination != expected.destination ||
         metadata.sector_size != expected.sector_size)) return false;
    return true;
}

std::string NewGenerationId() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    char value[80];
    snprintf(value, sizeof(value), "%08lx-%08lx%08lx-%016llx",
             static_cast<unsigned long>(GetCurrentProcessId()),
             static_cast<unsigned long>(time.dwHighDateTime),
             static_cast<unsigned long>(time.dwLowDateTime),
             static_cast<unsigned long long>(GetTickCount64()));
    return value;
}

}  // namespace

bool RescueMap::Load(const std::string& path) {
    std::vector<Range> loaded;
    MapMetadata expected{generation_id_, source_identity_, destination_identity_, sector_size_};
    MapMetadata metadata;
    loaded_from_backup_ = false;
    if (!LoadValidated(path, total_size_, expected, loaded, metadata)) {
        loaded.clear();
        metadata = {};
        if (!LoadValidated(path + ".bak", total_size_, expected, loaded, metadata)) return false;
        loaded_from_backup_ = true;
    }
    ranges_ = std::move(loaded);
    if (!metadata.generation.empty()) generation_id_ = std::move(metadata.generation);
    if (source_identity_.empty()) {
        source_identity_ = std::move(metadata.source);
        destination_identity_ = std::move(metadata.destination);
        sector_size_ = metadata.sector_size;
    }
    return true;
}

void RescueMap::ConfigureIdentity(std::string source, std::string destination,
                                  uint32_t sector_size) {
    source_identity_ = std::move(source);
    destination_identity_ = std::move(destination);
    sector_size_ = sector_size;
    if (generation_id_.empty()) generation_id_ = NewGenerationId();
}

bool RescueMap::Save(const std::string& path) const {
    namespace fs = std::filesystem;
    const fs::path target(path);
    const fs::path temp(path + ".tmp");
    const fs::path backup(path + ".bak");

    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out.is_open()) return false;
        out << "#total " << total_size_ << "\n";
        if (!generation_id_.empty()) out << "#generation " << generation_id_ << "\n";
        if (!source_identity_.empty()) {
            out << "#source " << std::quoted(source_identity_) << "\n";
            out << "#destination " << std::quoted(destination_identity_) << "\n";
            out << "#sector " << sector_size_ << "\n";
        }
        for (const auto& r : ranges_) {
            out << r.offset << " " << r.size << " " << static_cast<char>(r.status) << "\n";
        }
        out.flush();
        if (!out.good()) return false;
    }
    HANDLE temp_handle = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temp_handle == INVALID_HANDLE_VALUE || !FlushFileBuffers(temp_handle)) {
        if (temp_handle != INVALID_HANDLE_VALUE) CloseHandle(temp_handle);
        std::error_code remove_ec;
        fs::remove(temp, remove_ec);
        return false;
    }
    if (!CloseHandle(temp_handle)) return false;

    std::error_code ec;
    if (fs::exists(target, ec)) {
        fs::remove(backup, ec);
        ec.clear();
        fs::rename(target, backup, ec);
        if (ec) {
            fs::remove(temp, ec);
            return false;
        }
    }
    ec.clear();
    fs::rename(temp, target, ec);
    if (ec) {
        std::error_code restore_ec;
        if (fs::exists(backup, restore_ec)) fs::rename(backup, target, restore_ec);
        fs::remove(temp, restore_ec);
        return false;
    }
    return true;
}

void RescueMap::SetRange(uint64_t offset, uint64_t size, RangeStatus status) {
    if (size == 0 || offset >= total_size_) return;
    size = std::min(size, total_size_ - offset);
    uint64_t end = offset + size;

    std::vector<Range> result;
    result.reserve(ranges_.size() + 2);

    for (const auto& r : ranges_) {
        uint64_t r_end = r.offset + r.size;
        if (r_end <= offset || r.offset >= end) {
            // No overlap.
            result.push_back(r);
            continue;
        }
        // Split off the part before the new range.
        if (r.offset < offset) {
            result.push_back({r.offset, offset - r.offset, r.status});
        }
        // Split off the part after the new range.
        if (r_end > end) {
            result.push_back({end, r_end - end, r.status});
        }
    }
    result.push_back({offset, size, status});

    std::sort(result.begin(), result.end(),
              [](const Range& a, const Range& b) { return a.offset < b.offset; });

    // Merge adjacent ranges with the same status.
    std::vector<Range> merged;
    for (const auto& r : result) {
        if (!merged.empty() && merged.back().status == r.status &&
            merged.back().offset + merged.back().size == r.offset) {
            merged.back().size += r.size;
        } else {
            merged.push_back(r);
        }
    }
    ranges_ = std::move(merged);
}

void RescueMap::MarkRescued(uint64_t offset, uint64_t size) {
    SetRange(offset, size, RangeStatus::Rescued);
}

void RescueMap::MarkBad(uint64_t offset, uint64_t size) {
    SetRange(offset, size, RangeStatus::Bad);
}

void RescueMap::MarkDeferred(uint64_t offset, uint64_t size) {
    SetRange(offset, size, RangeStatus::Deferred);
}

void RescueMap::MarkNotTried(uint64_t offset, uint64_t size) {
    SetRange(offset, size, RangeStatus::NotTried);
}

Range RescueMap::NextPending(uint64_t max_chunk, bool reverse) const {
    if (reverse) {
        for (auto it = ranges_.rbegin(); it != ranges_.rend(); ++it) {
            if (it->status == RangeStatus::NotTried) {
                const uint64_t size = std::min(it->size, max_chunk);
                return {it->offset + it->size - size, size, RangeStatus::NotTried};
            }
        }
    } else {
        for (const auto& r : ranges_) {
            if (r.status == RangeStatus::NotTried) {
                uint64_t size = std::min(r.size, max_chunk);
                return {r.offset, size, RangeStatus::NotTried};
            }
        }
    }
    return {0, 0, RangeStatus::NotTried};
}

std::vector<Range> RescueMap::RangesWithStatus(RangeStatus status, bool reverse) const {
    std::vector<Range> result;
    for (const auto& r : ranges_)
        if (r.status == status) result.push_back(r);
    if (reverse) std::reverse(result.begin(), result.end());
    return result;
}

uint64_t RescueMap::TotalRescued() const {
    uint64_t total = 0;
    for (const auto& r : ranges_)
        if (r.status == RangeStatus::Rescued) total += r.size;
    return total;
}

uint64_t RescueMap::TotalBad() const {
    uint64_t total = 0;
    for (const auto& r : ranges_)
        if (r.status == RangeStatus::Bad) total += r.size;
    return total;
}

bool RescueMap::IsFullyRescued(uint64_t offset, uint64_t size) const {
    return UnrescuedBytes(offset, size) == 0;
}

uint64_t RescueMap::UnrescuedBytes(uint64_t offset, uint64_t size) const {
    if (size == 0) return 0;
    if (offset >= total_size_ || size > total_size_ - offset) return size;
    const uint64_t end = offset + size;
    uint64_t cursor = offset;
    uint64_t unrescued = 0;
    for (const auto& r : ranges_) {
        const uint64_t r_end = r.offset + r.size;
        if (r_end <= cursor) continue;
        if (r.offset >= end) break;
        if (r.offset > cursor) unrescued += std::min(end, r.offset) - cursor;
        const uint64_t overlap_start = std::max(cursor, r.offset);
        const uint64_t overlap_end = std::min(end, r_end);
        if (overlap_end > overlap_start && r.status != RangeStatus::Rescued)
            unrescued += overlap_end - overlap_start;
        cursor = std::max(cursor, overlap_end);
        if (cursor == end) break;
    }
    if (cursor < end) unrescued += end - cursor;
    return unrescued;
}

}  // namespace recovery

// exfat-parser: walks an exFAT volume image and recovers files with their
// ORIGINAL names and directory structure, by parsing the boot sector, FAT,
// and directory entry sets directly (does not rely on the OS mounting the
// volume, which matters when the volume is too damaged to mount).
//
// Run this on the .img produced by imager, never on the raw disk.
//
// Usage:
//   exfat_recovery.exe disk.img D:\recovered [--include-deleted]
//
// Output:
//   D:\recovered\<original tree>\...      recovered files
//   D:\recovered\manifest.csv             original_path,size,status

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "rescue_map.h"
#include "random_access_reader.h"
#include "sha256.h"
#include "win32_paths.h"

namespace fs = std::filesystem;

namespace {

#pragma pack(push, 1)
struct BootSector {
    uint8_t jump_boot[3];
    char fs_name[8];
    uint8_t must_be_zero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;       // sectors
    uint32_t fat_length;       // sectors
    uint32_t cluster_heap_offset;  // sectors
    uint32_t cluster_count;
    uint32_t first_cluster_of_root_dir;
    uint32_t volume_serial_number;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t bytes_per_sector_shift;
    uint8_t sectors_per_cluster_shift;
    uint8_t number_of_fats;
    uint8_t drive_select;
    uint8_t percent_in_use;
    uint8_t reserved[7];
    // BootCode[390], BootSignature[2] follow but are unused here.
};
#pragma pack(pop)
static_assert(sizeof(BootSector) == 120, "unexpected BootSector layout");

#pragma pack(push, 1)
struct DirEntryRaw {
    uint8_t entry_type;
    uint8_t data[31];
};
#pragma pack(pop)
static_assert(sizeof(DirEntryRaw) == 32, "dir entry must be 32 bytes");

constexpr uint8_t kEntryTypeFile = 0x85;
constexpr uint8_t kEntryTypeStreamExt = 0xC0;
constexpr uint8_t kEntryTypeFileName = 0xC1;
constexpr uint8_t kEntryTypeAllocationBitmap = 0x81;
constexpr uint8_t kEntryTypeUpcaseTable = 0x82;
constexpr uint8_t kInUseBit = 0x80;

constexpr uint16_t kAttrDirectory = 0x10;

constexpr uint32_t kFatEntryEOF = 0xFFFFFFFF;
constexpr uint32_t kFatEntryBad = 0xFFFFFFF7;

struct Volume {
    recovery::RandomAccessReader img;
    BootSector boot{};
    uint64_t image_size = 0;
    uint64_t volume_offset = 0;
    uint64_t volume_size = 0;
    uint64_t bytes_per_sector = 0;
    uint64_t bytes_per_cluster = 0;
    uint64_t cluster_heap_byte_offset = 0;
    uint64_t fat_byte_offset = 0;
    recovery::RescueMap* rescue_map = nullptr;
    std::vector<uint8_t> allocation_bitmap;
    bool allocation_bitmap_valid = false;
    std::vector<uint16_t> upcase_table;
    bool upcase_table_valid = false;
    bool used_backup_boot = false;
    bool boot_checksum_valid = true;

    uint64_t ClusterOffset(uint32_t cluster) const {
        return cluster_heap_byte_offset + (static_cast<uint64_t>(cluster) - 2) * bytes_per_cluster;
    }

    // Bytes at or beyond this offset were never stored by the device (a
    // fake-capacity flash drive reports far more space than it has and reads
    // back zeros there). 0 = no limit.
    uint64_t real_capacity = 0;

    uint64_t BeyondCapacityBytes(uint64_t offset, uint64_t size) const {
        if (real_capacity == 0 || offset + size <= real_capacity) return 0;
        return offset >= real_capacity ? size : offset + size - real_capacity;
    }

    bool RangeFullyRescued(uint64_t offset, uint64_t size) const {
        if (BeyondCapacityBytes(offset, size) != 0) return false;
        return !rescue_map || rescue_map->IsFullyRescued(offset, size);
    }

    uint64_t UnrescuedBytes(uint64_t offset, uint64_t size) const {
        const uint64_t beyond = BeyondCapacityBytes(offset, size);
        const uint64_t within = size - beyond;
        const uint64_t unrescued = rescue_map && within != 0
            ? rescue_map->UnrescuedBytes(offset, within) : 0;
        return unrescued + beyond;
    }

    bool ClusterAllocated(uint32_t cluster) const {
        if (!allocation_bitmap_valid || cluster < 2 || cluster >= boot.cluster_count + 2)
            return true;
        const uint64_t bit = static_cast<uint64_t>(cluster) - 2;
        return (allocation_bitmap[bit / 8] & (1u << (bit % 8))) != 0;
    }

    bool ReadCluster(uint32_t cluster, std::vector<uint8_t>& buf,
                     bool* source_fully_rescued = nullptr) {
        if (cluster < 2 || cluster >= boot.cluster_count + 2 ||
            bytes_per_cluster > std::numeric_limits<size_t>::max()) return false;
        const uint64_t offset = ClusterOffset(cluster);
        if (offset < volume_offset || bytes_per_cluster > image_size - offset) return false;
        if (source_fully_rescued) {
            *source_fully_rescued = !rescue_map ||
                rescue_map->IsFullyRescued(offset, bytes_per_cluster);
        }
        buf.resize(bytes_per_cluster);
        return img.ReadAt(offset, buf.data(), static_cast<size_t>(bytes_per_cluster));
    }

    uint32_t FatEntry(uint32_t cluster) {
        if (cluster >= boot.cluster_count + 2) return kFatEntryBad;
        uint64_t off = fat_byte_offset + static_cast<uint64_t>(cluster) * 4;
        uint32_t val = 0;
        if (!img.ReadAt(off, &val, 4)) return kFatEntryBad;
        return val;
    }
};

bool ReadAt(recovery::RandomAccessReader& in, uint64_t offset, void* data, size_t size) {
    return in.ReadAt(offset, data, size);
}

bool HasExfatSignature(recovery::RandomAccessReader& in, uint64_t offset, uint64_t image_size) {
    char sector[512]{};
    return offset <= image_size && sizeof(sector) <= image_size - offset &&
           ReadAt(in, offset, sector, sizeof(sector)) &&
           memcmp(sector + 3, "EXFAT   ", 8) == 0;
}

bool CandidateHasExfatBoot(recovery::RandomAccessReader& in, uint64_t offset, uint64_t image_size) {
    if (HasExfatSignature(in, offset, image_size)) return true;
    for (uint8_t shift = 9; shift <= 12; ++shift) {
        const uint64_t backup = offset + 12ull * (1ull << shift);
        if (HasExfatSignature(in, backup, image_size)) return true;
    }
    return false;
}

// Partition tables store LBAs in units of the device's logical sector size,
// which an image does not record. Try the common sizes (512-byte and 4Kn).
constexpr uint64_t kCandidateSectorSizes[] = {512, 4096};

std::optional<uint64_t> FindExfatVolume(recovery::RandomAccessReader& in, uint64_t image_size) {
    if (CandidateHasExfatBoot(in, 0, image_size)) return 0;

    uint8_t mbr[512]{};
    if (!ReadAt(in, 0, mbr, sizeof(mbr))) return std::nullopt;
    for (uint64_t sector : kCandidateSectorSizes) {
        for (int i = 0; i < 4; ++i) {
            const uint8_t* entry = mbr + 446 + i * 16;
            uint32_t first_lba = 0;
            memcpy(&first_lba, entry + 8, sizeof(first_lba));
            const uint64_t offset = static_cast<uint64_t>(first_lba) * sector;
            if (first_lba != 0 && CandidateHasExfatBoot(in, offset, image_size)) return offset;
        }
    }

    for (uint64_t sector : kCandidateSectorSizes) {
        uint8_t gpt[512]{};
        if (sector > image_size || sizeof(gpt) > image_size - sector) continue;
        if (!ReadAt(in, sector, gpt, sizeof(gpt)) || memcmp(gpt, "EFI PART", 8) != 0)
            continue;
        uint64_t entries_lba = 0;
        uint32_t entry_count = 0, entry_size = 0;
        memcpy(&entries_lba, gpt + 72, 8);
        memcpy(&entry_count, gpt + 80, 4);
        memcpy(&entry_size, gpt + 84, 4);
        if (entry_count == 0 || entry_count > 4096 || entry_size < 128 || entry_size > 4096 ||
            entries_lba > std::numeric_limits<uint64_t>::max() / sector)
            continue;
        std::vector<uint8_t> entry(entry_size);
        for (uint32_t i = 0; i < entry_count; ++i) {
            const uint64_t entry_offset = entries_lba * sector + static_cast<uint64_t>(i) * entry_size;
            if (entry_offset > image_size || entry_size > image_size - entry_offset ||
                !ReadAt(in, entry_offset, entry.data(), entry.size())) break;
            bool unused = true;
            for (size_t j = 0; j < 16; ++j) unused &= entry[j] == 0;
            if (unused) continue;
            uint64_t first_lba = 0;
            memcpy(&first_lba, entry.data() + 32, 8);
            if (first_lba <= std::numeric_limits<uint64_t>::max() / sector) {
                const uint64_t offset = first_lba * sector;
                if (CandidateHasExfatBoot(in, offset, image_size)) return offset;
            }
        }
    }
    return std::nullopt;
}

bool BootRegionChecksumValid(recovery::RandomAccessReader& in, uint64_t offset,
                             uint64_t bytes_per_sector, uint64_t image_size) {
    const uint64_t region_size = 12 * bytes_per_sector;
    if (offset > image_size || region_size > image_size - offset ||
        region_size > std::numeric_limits<size_t>::max()) return false;
    std::vector<uint8_t> region(static_cast<size_t>(region_size));
    if (!ReadAt(in, offset, region.data(), region.size())) return false;

    uint32_t checksum = 0;
    const size_t checked_size = static_cast<size_t>(11 * bytes_per_sector);
    for (size_t i = 0; i < checked_size; ++i) {
        if (i == 106 || i == 107 || i == 112) continue;
        checksum = ((checksum & 1) ? 0x80000000u : 0) + (checksum >> 1) + region[i];
    }
    for (size_t i = checked_size; i + 4 <= region.size(); i += 4) {
        uint32_t stored = 0;
        memcpy(&stored, region.data() + i, 4);
        if (stored != checksum) return false;
    }
    return true;
}

bool BootGeometryPlausible(const BootSector& boot, uint64_t volume_offset,
                           uint64_t image_size) {
    if (std::string(boot.fs_name, 8) != "EXFAT   " ||
        !std::all_of(std::begin(boot.must_be_zero), std::end(boot.must_be_zero),
                     [](uint8_t value) { return value == 0; }) ||
        boot.bytes_per_sector_shift < 9 || boot.bytes_per_sector_shift > 12 ||
        static_cast<unsigned>(boot.bytes_per_sector_shift) +
                boot.sectors_per_cluster_shift > 25 ||
        boot.number_of_fats < 1 || boot.number_of_fats > 2 ||
        boot.fat_offset == 0 || boot.fat_length == 0 ||
        boot.cluster_heap_offset <= boot.fat_offset || boot.cluster_count == 0 ||
        boot.first_cluster_of_root_dir < 2 ||
        static_cast<uint64_t>(boot.first_cluster_of_root_dir) >=
            static_cast<uint64_t>(boot.cluster_count) + 2) {
        return false;
    }

    const uint64_t bytes_per_sector = 1ull << boot.bytes_per_sector_shift;
    const uint64_t bytes_per_cluster = bytes_per_sector << boot.sectors_per_cluster_shift;
    if (boot.volume_length > std::numeric_limits<uint64_t>::max() / bytes_per_sector)
        return false;
    const uint64_t volume_size = boot.volume_length * bytes_per_sector;
    if (volume_offset > image_size || volume_size > image_size - volume_offset)
        return false;
    const uint64_t fat_end = static_cast<uint64_t>(boot.fat_offset) +
                             static_cast<uint64_t>(boot.fat_length) * boot.number_of_fats;
    const uint64_t heap_start = static_cast<uint64_t>(boot.cluster_heap_offset) * bytes_per_sector;
    if (fat_end > boot.cluster_heap_offset || heap_start > volume_size ||
        static_cast<uint64_t>(boot.cluster_count) >
            (volume_size - heap_start) / bytes_per_cluster) {
        return false;
    }
    return true;
}

bool LoadValidatedBoot(recovery::RandomAccessReader& in, uint64_t volume_offset, uint64_t image_size,
                       BootSector& boot, bool& used_backup, bool& checksum_valid) {
    std::vector<uint64_t> candidates{volume_offset};
    for (uint8_t shift = 9; shift <= 12; ++shift)
        candidates.push_back(volume_offset + 12ull * (1ull << shift));

    for (size_t i = 0; i < candidates.size(); ++i) {
        BootSector candidate{};
        if (candidates[i] > image_size || sizeof(candidate) > image_size - candidates[i] ||
            !ReadAt(in, candidates[i], &candidate, sizeof(candidate)) ||
            std::string(candidate.fs_name, 8) != "EXFAT   " ||
            candidate.bytes_per_sector_shift < 9 || candidate.bytes_per_sector_shift > 12) {
            continue;
        }
        const uint64_t bps = 1ull << candidate.bytes_per_sector_shift;
        const uint64_t expected = i == 0 ? volume_offset : volume_offset + 12 * bps;
        if (candidates[i] != expected ||
            !BootRegionChecksumValid(in, candidates[i], bps, image_size)) continue;
        boot = candidate;
        used_backup = i != 0;
        checksum_valid = true;
        return true;
    }

    // Recovery must remain possible when one of the extended boot sectors or
    // the checksum sector is unreadable.  Only fall back after the strict pass,
    // and require the boot sector's reserved bytes and all geometry/ranges to
    // be internally consistent with the actual source size.
    for (size_t i = 0; i < candidates.size(); ++i) {
        BootSector candidate{};
        if (candidates[i] > image_size || sizeof(candidate) > image_size - candidates[i] ||
            !ReadAt(in, candidates[i], &candidate, sizeof(candidate)) ||
            !BootGeometryPlausible(candidate, volume_offset, image_size)) {
            continue;
        }
        const uint64_t bps = 1ull << candidate.bytes_per_sector_shift;
        const uint64_t expected = i == 0 ? volume_offset : volume_offset + 12 * bps;
        if (candidates[i] != expected) continue;
        boot = candidate;
        used_backup = i != 0;
        checksum_valid = false;
        return true;
    }
    return false;
}

void PrintBootDiagnostics(recovery::RandomAccessReader& in, uint64_t volume_offset,
                          uint64_t image_size) {
    fprintf(stderr, "boot diagnostics (all offsets are bytes):\n");
    std::vector<uint64_t> candidates{volume_offset};
    for (uint8_t shift = 9; shift <= 12; ++shift)
        candidates.push_back(volume_offset + 12ull * (1ull << shift));
    for (const uint64_t offset : candidates) {
        BootSector candidate{};
        if (offset > image_size || sizeof(candidate) > image_size - offset ||
            !ReadAt(in, offset, &candidate, sizeof(candidate))) {
            fprintf(stderr, "  offset %llu: read failed (Windows error %lu)\n",
                    (unsigned long long)offset, in.LastErrorCode());
            continue;
        }
        char signature[9]{};
        for (size_t i = 0; i < 8; ++i) {
            const unsigned char c = static_cast<unsigned char>(candidate.fs_name[i]);
            signature[i] = c >= 32 && c <= 126 ? static_cast<char>(c) : '.';
        }
        const bool zero_reserved =
            std::all_of(std::begin(candidate.must_be_zero), std::end(candidate.must_be_zero),
                        [](uint8_t value) { return value == 0; });
        fprintf(stderr,
                "  offset %llu: fs='%s' fshex=%02X%02X%02X%02X%02X%02X%02X%02X "
                "zero=%s bpsShift=%u spcShift=%u fats=%u fatOff=%u fatLen=%u "
                "heapOff=%u clusters=%u root=%u volumeSectors=%llu\n",
                (unsigned long long)offset, signature,
                static_cast<unsigned char>(candidate.fs_name[0]),
                static_cast<unsigned char>(candidate.fs_name[1]),
                static_cast<unsigned char>(candidate.fs_name[2]),
                static_cast<unsigned char>(candidate.fs_name[3]),
                static_cast<unsigned char>(candidate.fs_name[4]),
                static_cast<unsigned char>(candidate.fs_name[5]),
                static_cast<unsigned char>(candidate.fs_name[6]),
                static_cast<unsigned char>(candidate.fs_name[7]),
                zero_reserved ? "yes" : "no", candidate.bytes_per_sector_shift,
                candidate.sectors_per_cluster_shift, candidate.number_of_fats,
                candidate.fat_offset, candidate.fat_length, candidate.cluster_heap_offset,
                candidate.cluster_count, candidate.first_cluster_of_root_dir,
                (unsigned long long)candidate.volume_length);
    }
}

bool OpenVolume(const std::string& path, Volume& vol, std::optional<uint64_t> requested_offset,
                uint64_t source_size_override) {
    if (!vol.img.Open(fs::path(path), source_size_override)) {
        fprintf(stderr, "cannot open source %s (Windows error %lu)\n",
                path.c_str(), vol.img.LastErrorCode());
        return false;
    }
    vol.image_size = vol.img.Size();
    if (vol.image_size == 0) return false;
    auto detected = requested_offset ? requested_offset : FindExfatVolume(vol.img, vol.image_size);
    if (!detected || *detected > vol.image_size || sizeof(BootSector) > vol.image_size - *detected) {
        const DWORD read_error = vol.img.LastErrorCode();
        if (read_error != ERROR_SUCCESS) {
            fprintf(stderr,
                    "could not read exFAT boot/partition metadata from %s (Windows error %lu)\n",
                    path.c_str(), read_error);
        } else {
            fprintf(stderr, "could not locate an exFAT volume; use --offset <bytes> if needed\n");
        }
        return false;
    }
    vol.volume_offset = *detected;
    if (!LoadValidatedBoot(vol.img, vol.volume_offset, vol.image_size, vol.boot,
                           vol.used_backup_boot, vol.boot_checksum_valid)) {
        fprintf(stderr, "both primary and backup exFAT boot regions are invalid\n");
        PrintBootDiagnostics(vol.img, vol.volume_offset, vol.image_size);
        return false;
    }
    if (vol.boot.bytes_per_sector_shift < 9 || vol.boot.bytes_per_sector_shift > 12 ||
        static_cast<unsigned>(vol.boot.bytes_per_sector_shift) + vol.boot.sectors_per_cluster_shift > 25 ||
        vol.boot.cluster_count == 0 || vol.boot.first_cluster_of_root_dir < 2 ||
        vol.boot.first_cluster_of_root_dir >= vol.boot.cluster_count + 2 ||
        (vol.boot.number_of_fats != 1 && vol.boot.number_of_fats != 2)) {
        fprintf(stderr, "invalid or damaged exFAT geometry\n");
        return false;
    }
    vol.bytes_per_sector = 1ull << vol.boot.bytes_per_sector_shift;
    vol.bytes_per_cluster = vol.bytes_per_sector << vol.boot.sectors_per_cluster_shift;
    if (vol.boot.volume_length > std::numeric_limits<uint64_t>::max() / vol.bytes_per_sector) {
        fprintf(stderr, "exFAT volume size overflows\n");
        return false;
    }
    vol.volume_size = vol.boot.volume_length * vol.bytes_per_sector;
    if (vol.volume_size > vol.image_size - vol.volume_offset) {
        fprintf(stderr, "exFAT volume extends beyond the image\n");
        return false;
    }
    vol.cluster_heap_byte_offset = vol.volume_offset + static_cast<uint64_t>(vol.boot.cluster_heap_offset) * vol.bytes_per_sector;
    vol.fat_byte_offset = vol.volume_offset + static_cast<uint64_t>(vol.boot.fat_offset) * vol.bytes_per_sector;
    if (vol.boot.number_of_fats == 2 && (vol.boot.volume_flags & 1))
        vol.fat_byte_offset += static_cast<uint64_t>(vol.boot.fat_length) * vol.bytes_per_sector;
    const uint64_t heap_size = static_cast<uint64_t>(vol.boot.cluster_count) * vol.bytes_per_cluster;
    if (vol.cluster_heap_byte_offset > vol.image_size || heap_size > vol.image_size - vol.cluster_heap_byte_offset ||
        vol.fat_byte_offset > vol.image_size || static_cast<uint64_t>(vol.boot.fat_length) * vol.bytes_per_sector > vol.image_size - vol.fat_byte_offset) {
        fprintf(stderr, "exFAT FAT or cluster heap lies outside the image\n");
        return false;
    }

    printf("exFAT volume at byte %llu: %llu bytes/sector, %llu bytes/cluster, %u clusters, root cluster %u\n",
           (unsigned long long)vol.volume_offset,
           (unsigned long long)vol.bytes_per_sector, (unsigned long long)vol.bytes_per_cluster,
           vol.boot.cluster_count, vol.boot.first_cluster_of_root_dir);
    if (vol.used_backup_boot) fprintf(stderr, "warning: using validated backup exFAT boot region\n");
    if (!vol.boot_checksum_valid)
        fprintf(stderr, "warning: boot-region checksum is invalid or unreadable; using structurally validated boot geometry\n");
    return true;
}

// Returns the list of cluster indices belonging to a file/directory, given
// its first cluster. If no_fat_chain is true the clusters are contiguous
// (common for exFAT since it avoids fragmentation); otherwise follow the FAT.
std::vector<uint32_t> ClusterChain(Volume& vol, uint32_t first_cluster, bool no_fat_chain,
                                    uint64_t data_length) {
    std::vector<uint32_t> clusters;
    if (first_cluster < 2) return clusters;

    if (no_fat_chain) {
        uint64_t needed = (data_length + vol.bytes_per_cluster - 1) / vol.bytes_per_cluster;
        if (needed > vol.boot.cluster_count || first_cluster - 2 > vol.boot.cluster_count - needed)
            return {};
        for (uint64_t i = 0; i < needed; ++i) clusters.push_back(static_cast<uint32_t>(first_cluster + i));
        return clusters;
    }

    uint32_t cluster = first_cluster;
    size_t guard = 0;
    std::unordered_set<uint32_t> visited;
    while (cluster >= 2 && cluster < kFatEntryBad && guard < vol.boot.cluster_count + 16) {
        if (cluster >= vol.boot.cluster_count + 2 || !visited.insert(cluster).second) return {};
        clusters.push_back(cluster);
        cluster = vol.FatEntry(cluster);
        ++guard;
    }
    return clusters;
}

std::string Utf16ToUtf8(const std::u16string& in) {
    if (in.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(in.data()),
                                   static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(in.data()),
                         static_cast<int>(in.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// Sanitize a recovered name for use on the host filesystem (exFAT allows a
// few characters NTFS/Win32 paths don't).
std::string SanitizeName(std::string name) {
    for (char& c : name) {
        if (static_cast<unsigned char>(c) < 32 || c == '<' || c == '>' || c == ':' ||
            c == '"' || c == '|' || c == '?' || c == '*' || c == '/' || c == '\\')
            c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty() || name == "." || name == "..") name = "_unnamed";

    std::string base = name.substr(0, name.find('.'));
    for (char& c : base) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    static const std::unordered_set<std::string> reserved = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
        "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    if (reserved.contains(base)) name = "_" + name;
    return name;
}

struct RecoveredEntry {
    std::string path;  // relative, forward-slash separated
    uint64_t logical_size;
    uint64_t written_size;
    bool is_directory;
    std::string status;  // "complete", "partial", "deleted-complete", "deleted-partial", "skipped-no-space", "skipped-filtered"
    std::string destination;  // which --dest root this landed in, empty if none
    std::string source_extents;
    uint64_t damaged_bytes;
    std::string modified_date{};
    std::string sha256{};
};

fs::path VerificationPath(const fs::path& output) {
    return fs::path(output.string() + ".recovery.sha256");
}

std::optional<std::string> VerifyExistingOutput(const fs::path& output, uint64_t expected_size) {
    std::error_code ec;
    if (!fs::is_regular_file(output, ec) || ec || fs::file_size(output, ec) != expected_size || ec)
        return std::nullopt;
    std::ifstream sidecar(VerificationPath(output));
    uint64_t recorded_size = 0;
    std::string recorded_hash;
    if (!(sidecar >> recorded_size >> recorded_hash) || recorded_size != expected_size ||
        recorded_hash.size() != 64) return std::nullopt;
    const auto actual = recovery::Sha256File(output);
    if (!actual || *actual != recorded_hash) return std::nullopt;
    return actual;
}

bool WriteVerification(const fs::path& output, uint64_t size, const std::string& hash) {
    const fs::path target = VerificationPath(output);
    const fs::path temp(target.string() + ".tmp");
    {
        std::ofstream sidecar(temp, std::ios::trunc);
        if (!sidecar.is_open()) return false;
        sidecar << size << ' ' << hash << '\n';
        sidecar.flush();
        if (!sidecar.good()) return false;
    }
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    const bool closed = CloseHandle(handle) != FALSE;
    return flushed && closed && MoveFileExW(temp.c_str(), target.c_str(),
                                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

uint32_t ExfatDateKey(uint32_t timestamp) {
    const uint32_t day = (timestamp >> 16) & 0x1F;
    const uint32_t month = (timestamp >> 21) & 0x0F;
    const uint32_t year = 1980 + ((timestamp >> 25) & 0x7F);
    if (month < 1 || month > 12 || day < 1 || day > 31) return 0;
    return year * 10000 + month * 100 + day;
}

std::string DateString(uint32_t key) {
    if (key == 0) return {};
    char text[11];
    snprintf(text, sizeof(text), "%04u-%02u-%02u", key / 10000,
             (key / 100) % 100, key % 100);
    return text;
}

std::string CsvField(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"') escaped.push_back('"');
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

std::string MetadataConfidence(const std::string& status, const Volume& volume) {
    if (status.find("metadata-damaged") != std::string::npos ||
        status.find("unreadable") != std::string::npos ||
        status.find("write-error") != std::string::npos) return "low";
    if (volume.used_backup_boot || !volume.boot_checksum_valid ||
        (volume.boot.volume_flags & 0x0002) != 0 ||
        status.find("partial") != std::string::npos ||
        status.find("deleted") != std::string::npos ||
        status.find("unknown") != std::string::npos) return "medium";
    return "high";
}

// One output target with an optional capacity cap. When capacity is 0 the
// destination is unlimited (fills forever). Destinations are tried in the
// order given; a file goes to the first one with enough room left.
struct Destination {
    std::string label;   // e.g. "D:\recovered1" (used in manifest/log output)
    fs::path root;
    uint64_t capacity;    // bytes, 0 = unlimited
    uint64_t used = 0;
};

struct PendingFile {
    uint32_t first_cluster;
    bool no_fat_chain;
    uint64_t data_length;
    uint64_t valid_data_length;
    fs::path rel_path;
    bool in_use;
    bool metadata_valid;
    int priority;
    uint32_t modified_date_key;
    std::string modified_date;
};

bool EntrySetChecksumValid(const std::vector<DirEntryRaw>& entries, size_t first,
                           uint8_t secondary_count, bool deleted);

// True when a sector begins with a structurally valid exFAT file entry set
// (File -> Stream Extension -> File Name, checksum intact). Data clusters do
// not produce this pattern by accident, so it identifies the first cluster of
// a directory whose parent entry no longer points at it.
bool LooksLikeDirectoryStart(const std::vector<DirEntryRaw>& entries) {
    if (entries.size() < 3) return false;
    const uint8_t type = entries[0].entry_type;
    if ((type & ~kInUseBit) != (kEntryTypeFile & ~kInUseBit)) return false;
    const uint8_t secondary_count = entries[0].data[0];
    if (secondary_count < 2 || secondary_count > 18 || secondary_count >= entries.size()) return false;
    if ((entries[1].entry_type & ~kInUseBit) != (kEntryTypeStreamExt & ~kInUseBit)) return false;
    if ((entries[2].entry_type & ~kInUseBit) != (kEntryTypeFileName & ~kInUseBit)) return false;
    return EntrySetChecksumValid(entries, 0, secondary_count, (type & kInUseBit) == 0);
}

bool EntrySetChecksumValid(const std::vector<DirEntryRaw>& entries, size_t first,
                           uint8_t secondary_count, bool deleted) {
    if (first >= entries.size() || first + secondary_count >= entries.size()) return false;
    uint16_t stored = 0;
    memcpy(&stored, &entries[first].data[1], 2);
    uint16_t checksum = 0;
    for (size_t entry_index = 0; entry_index <= secondary_count; ++entry_index) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&entries[first + entry_index]);
        for (size_t byte_index = 0; byte_index < sizeof(DirEntryRaw); ++byte_index) {
            if (entry_index == 0 && (byte_index == 2 || byte_index == 3)) continue;
            uint8_t value = bytes[byte_index];
            // Deletion clears InUse on every entry but does not rewrite the
            // stored set checksum, so restore it for validation.
            if (deleted && byte_index == 0) value |= kInUseBit;
            checksum = static_cast<uint16_t>(((checksum & 1) ? 0x8000u : 0) +
                                             (checksum >> 1) + value);
        }
    }
    return checksum == stored;
}

uint16_t ExfatNameHash(const std::u16string& name, const std::vector<uint16_t>& upcase) {
    uint16_t hash = 0;
    for (char16_t character : name) {
        const uint16_t upper = upcase[static_cast<uint16_t>(character)];
        for (const uint8_t byte : {static_cast<uint8_t>(upper & 0xFF),
                                   static_cast<uint8_t>(upper >> 8)}) {
            hash = static_cast<uint16_t>(((hash & 1) ? 0x8000u : 0) +
                                         (hash >> 1) + byte);
        }
    }
    return hash;
}

// Simple case-insensitive substring/extension filters applied to files only
// (directories are always traversed so nested matches are still found).
struct Filters {
    std::vector<std::string> only_ext;        // lowercase, no dot, e.g. "jpg"
    std::vector<std::string> path_contains;   // lowercase substrings
    std::unordered_set<std::string> exact_paths;  // populated by --from-plan
    bool exact_paths_enabled = false;
    std::unordered_map<std::string, int> priorities;
    uint64_t min_size = 0;
    uint64_t max_size = std::numeric_limits<uint64_t>::max();
    uint32_t modified_after = 0;
    uint32_t modified_before = std::numeric_limits<uint32_t>::max();

    static std::string Lower(std::string s) {
        for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool Matches(const fs::path& rel_path, uint64_t size, uint32_t modified_date) const {
        std::string path_lower = Lower(rel_path.generic_string());
        if (size < min_size || size > max_size) return false;
        const bool date_filter_active = modified_after != 0 ||
            modified_before != std::numeric_limits<uint32_t>::max();
        if (date_filter_active && (modified_date == 0 || modified_date < modified_after ||
                                   modified_date > modified_before)) return false;
        if (exact_paths_enabled && !exact_paths.contains(path_lower)) return false;
        if (!only_ext.empty()) {
            std::string ext = Lower(rel_path.extension().string());
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            if (std::find(only_ext.begin(), only_ext.end(), ext) == only_ext.end()) return false;
        }
        if (!path_contains.empty()) {
            bool any = false;
            for (const auto& sub : path_contains) {
                if (path_lower.find(sub) != std::string::npos) { any = true; break; }
            }
            if (!any) return false;
        }
        return true;
    }

    int Priority(const fs::path& rel_path) const {
        const auto it = priorities.find(Lower(rel_path.generic_string()));
        return it == priorities.end() ? 100 : it->second;
    }
};

struct Recovery {
    Volume& vol;
    std::vector<Destination>& destinations;
    Filters filters;
    bool include_deleted;
    bool scan_only;
    fs::path source_path;
    std::vector<RecoveredEntry> manifest;
    std::unordered_map<std::string, uint32_t> output_names;
    std::vector<PendingFile> pending_files;
    uint64_t planned_extent_tasks = 0;
    uint64_t unique_extent_reads = 0;
    uint64_t source_data_bytes_read = 0;
    uint64_t deduplicated_extent_tasks = 0;

    // Returns the destination a file of this size should land in, preferring
    // destinations earlier in the list, or nullptr if none has room.
    Destination* PickDestination(uint64_t size) {
        for (auto& d : destinations) {
            if (size > std::numeric_limits<uint64_t>::max() - d.used) continue;
            if (d.capacity != 0 && d.used + size > d.capacity) continue;
            std::error_code ec;
            const auto info = fs::space(d.root, ec);
            if (!ec && info.available < size) continue;
            return &d;
        }
        return nullptr;
    }

    fs::path UniqueOutputPath(Destination& dest, const fs::path& rel_path) {
        fs::path candidate = rel_path;
        for (;;) {
            std::string key = Filters::Lower((dest.root / candidate).lexically_normal().generic_string());
            uint32_t& sequence = output_names[key];
            std::error_code ec;
            const fs::path full = dest.root / candidate;
            const bool exists = fs::exists(full, ec);
            if (sequence == 0 && !exists) {
                sequence = 1;
                return full;
            }
            const uint32_t suffix = ++sequence;
            const fs::path parent = rel_path.parent_path();
            const std::string stem = rel_path.stem().string();
            const std::string extension = rel_path.extension().string();
            candidate = parent / (stem + "~recovered_" + std::to_string(suffix) + extension);
        }
    }

    void TryLoadAllocationBitmap(const DirEntryRaw& entry) {
        const uint8_t bitmap_id = entry.data[0] & 1;
        const uint8_t active_id = vol.boot.number_of_fats == 2 ? (vol.boot.volume_flags & 1) : 0;
        if (bitmap_id != active_id) return;
        uint32_t first_cluster = 0;
        uint64_t data_length = 0;
        memcpy(&first_cluster, &entry.data[19], 4);
        memcpy(&data_length, &entry.data[23], 8);
        const uint64_t needed = (static_cast<uint64_t>(vol.boot.cluster_count) + 7) / 8;
        if (data_length < needed || data_length > needed + vol.bytes_per_cluster) return;
        const auto clusters = ClusterChain(vol, first_cluster, false, data_length);
        if (clusters.empty()) return;
        std::vector<uint8_t> bitmap;
        bitmap.reserve(static_cast<size_t>(needed));
        std::vector<uint8_t> cluster_data;
        for (uint32_t cluster : clusters) {
            bool fully_rescued = true;
            if (!vol.ReadCluster(cluster, cluster_data, &fully_rescued) || !fully_rescued) return;
            const size_t take = static_cast<size_t>(std::min<uint64_t>(needed - bitmap.size(), cluster_data.size()));
            bitmap.insert(bitmap.end(), cluster_data.begin(), cluster_data.begin() + take);
            if (bitmap.size() == needed) break;
        }
        if (bitmap.size() == needed) {
            vol.allocation_bitmap = std::move(bitmap);
            vol.allocation_bitmap_valid = true;
        }
    }

    void TryLoadUpcaseTable(const DirEntryRaw& entry) {
        uint32_t expected_checksum = 0, first_cluster = 0;
        uint64_t data_length = 0;
        memcpy(&expected_checksum, &entry.data[3], 4);
        memcpy(&first_cluster, &entry.data[19], 4);
        memcpy(&data_length, &entry.data[23], 8);
        if (data_length < 4 || data_length > 128ull * 1024 || (data_length & 1)) return;
        const auto clusters = ClusterChain(vol, first_cluster, false, data_length);
        if (clusters.empty()) return;
        std::vector<uint8_t> encoded;
        encoded.reserve(static_cast<size_t>(data_length));
        std::vector<uint8_t> cluster_data;
        for (uint32_t cluster : clusters) {
            bool fully_rescued = true;
            if (!vol.ReadCluster(cluster, cluster_data, &fully_rescued) || !fully_rescued) return;
            const size_t take = static_cast<size_t>(std::min<uint64_t>(
                data_length - encoded.size(), cluster_data.size()));
            encoded.insert(encoded.end(), cluster_data.begin(), cluster_data.begin() + take);
            if (encoded.size() == data_length) break;
        }
        if (encoded.size() != data_length) return;
        uint32_t checksum = 0;
        for (uint8_t byte : encoded)
            checksum = ((checksum & 1) ? 0x80000000u : 0) + (checksum >> 1) + byte;
        if (checksum != expected_checksum) return;

        std::vector<uint16_t> table;
        table.reserve(65536);
        for (size_t i = 0; i + 1 < encoded.size() && table.size() < 65536; i += 2) {
            uint16_t value = 0;
            memcpy(&value, encoded.data() + i, 2);
            if (value != 0xFFFF) {
                table.push_back(value);
                continue;
            }
            if (i + 3 >= encoded.size()) return;
            uint16_t count = 0;
            memcpy(&count, encoded.data() + i + 2, 2);
            i += 2;
            if (count == 0 || count > 65536 - table.size()) return;
            for (uint32_t n = 0; n < count; ++n)
                table.push_back(static_cast<uint16_t>(table.size()));
        }
        if (table.size() != 65536) return;
        vol.upcase_table = std::move(table);
        vol.upcase_table_valid = true;
    }

    // Every cluster that has been parsed as directory data, so the lost-directory
    // search can skip what the live tree already covers.
    std::unordered_set<uint32_t> directory_clusters;

    void WalkDirectory(uint32_t first_cluster, bool no_fat_chain, uint64_t dir_data_length,
                        const fs::path& rel_path, int depth) {
        if (depth > 64) return;  // guard against corrupted cyclic chains
        auto clusters = ClusterChain(vol, first_cluster, no_fat_chain, dir_data_length);
        // A directory whose chain the FAT no longer describes (lost directory
        // found by signature) is still readable as its single first cluster.
        if (clusters.empty() && first_cluster >= 2 && first_cluster < vol.boot.cluster_count + 2 &&
            dir_data_length == 0) {
            clusters.push_back(first_cluster);
        }
        for (uint32_t c : clusters) directory_clusters.insert(c);
        const std::string dir_label = rel_path.empty() ? std::string("/") : rel_path.generic_string();
        if (clusters.empty()) {
            fprintf(stderr, "warning: directory '%s' has no usable cluster chain (first cluster %u, "
                    "length %llu); its contents cannot be listed\n", dir_label.c_str(), first_cluster,
                    static_cast<unsigned long long>(dir_data_length));
            return;
        }
        fprintf(stderr, "  dir %s (%zu clusters at cluster %u)\n", dir_label.c_str(), clusters.size(),
                first_cluster);

        std::vector<uint8_t> buf;
        std::vector<DirEntryRaw> entries;
        for (uint32_t c : clusters) {
            if (!vol.ReadCluster(c, buf)) {
                fprintf(stderr, "warning: read failed for directory '%s' at cluster %u (byte offset %llu); "
                        "entries after this point are lost\n", dir_label.c_str(), c,
                        static_cast<unsigned long long>(vol.ClusterOffset(c)));
                break;
            }
            size_t count = buf.size() / sizeof(DirEntryRaw);
            const auto* p = reinterpret_cast<const DirEntryRaw*>(buf.data());
            entries.insert(entries.end(), p, p + count);
        }

        {
            size_t zero = 0, file_live = 0, file_deleted = 0, stream = 0, name = 0, other = 0;
            for (const auto& entry : entries) {
                const uint8_t t = entry.entry_type;
                if (t == 0) ++zero;
                else if (t == kEntryTypeFile) ++file_live;
                else if (t == (kEntryTypeFile & ~kInUseBit)) ++file_deleted;
                else if ((t & ~kInUseBit) == (kEntryTypeStreamExt & ~kInUseBit)) ++stream;
                else if ((t & ~kInUseBit) == (kEntryTypeFileName & ~kInUseBit)) ++name;
                else ++other;
            }
            if (file_live + file_deleted == 0) fprintf(stderr, "      entries: %zu total, %zu empty, %zu file(live), %zu file(deleted), "
                    "%zu stream, %zu name, %zu other; first bytes:", entries.size(), zero, file_live,
                    file_deleted, stream, name, other);
            for (size_t k = 0; k < 16 && k < buf.size(); ++k) fprintf(stderr, " %02x", buf[k]);
            fprintf(stderr, "\n");
        }

        if (depth == 0) {
            for (const auto& entry : entries) {
                if (entry.entry_type == kEntryTypeAllocationBitmap) TryLoadAllocationBitmap(entry);
                else if (entry.entry_type == kEntryTypeUpcaseTable) TryLoadUpcaseTable(entry);
            }
        }

        for (size_t i = 0; i < entries.size(); ++i) {
            uint8_t type = entries[i].entry_type;
            if (depth == 0 && type == kEntryTypeAllocationBitmap) {
                continue;
            }
            if (depth == 0 && type == kEntryTypeUpcaseTable) continue;
            bool in_use = (type & kInUseBit) != 0;
            uint8_t bare_type = type & ~kInUseBit;

            if (bare_type != (kEntryTypeFile & ~kInUseBit)) continue;
            if (!in_use && !include_deleted) continue;

            uint8_t secondary_count = entries[i].data[0];
            // File Directory Entry layout (data[] is entry bytes[1..31]):
            // [0]=SecondaryCount [1..3)=SetChecksum [3..5)=FileAttributes ...
            uint16_t attrs;
            memcpy(&attrs, &entries[i].data[3], 2);
            bool is_dir = (attrs & kAttrDirectory) != 0;
            uint32_t modified_timestamp = 0;
            memcpy(&modified_timestamp, &entries[i].data[11], 4);
            const uint32_t modified_date_key = ExfatDateKey(modified_timestamp);
            const std::string modified_date = DateString(modified_date_key);

            if (i + secondary_count >= entries.size() || secondary_count < 2) continue;
            bool entry_set_valid = EntrySetChecksumValid(entries, i, secondary_count, !in_use);

            const DirEntryRaw& stream = entries[i + 1];
            uint8_t stream_bare = stream.entry_type & ~kInUseBit;
            if (stream_bare != (kEntryTypeStreamExt & ~kInUseBit)) continue;

            uint8_t general_flags = stream.data[0];
            bool no_fat_chain_flag = (general_flags & 0x02) != 0;
            uint8_t name_length = stream.data[2];
            uint64_t data_length;
            uint64_t valid_data_length;
            // Field layout inside stream.data (data[] is entry bytes[1..31]):
            // [0]=GeneralSecondaryFlags [1]=Reserved1 [2]=NameLength [3..5)=NameHash
            // [5..7)=Reserved2 [7..15)=ValidDataLength [15..19)=Reserved3
            // [19..23)=FirstCluster [23..31)=DataLength
            uint32_t first_cluster;
            memcpy(&first_cluster, &stream.data[19], 4);
            memcpy(&data_length, &stream.data[23], 8);
            memcpy(&valid_data_length, &stream.data[7], 8);
            entry_set_valid = entry_set_valid && valid_data_length <= data_length;

            std::u16string name;
            size_t name_entries_needed = (name_length + 14) / 15;
            for (size_t n = 0; n < name_entries_needed && (i + 2 + n) < entries.size(); ++n) {
                const DirEntryRaw& ne = entries[i + 2 + n];
                uint8_t ne_bare = ne.entry_type & ~kInUseBit;
                if (ne_bare != (kEntryTypeFileName & ~kInUseBit)) break;
                const char16_t* chars = reinterpret_cast<const char16_t*>(&ne.data[1]);
                size_t take = std::min<size_t>(15, name_length - name.size());
                name.append(chars, take);
            }
            entry_set_valid = entry_set_valid && name.size() == name_length;
            if (vol.upcase_table_valid && name.size() == name_length) {
                uint16_t stored_name_hash = 0;
                memcpy(&stored_name_hash, &stream.data[3], 2);
                entry_set_valid = entry_set_valid &&
                    stored_name_hash == ExfatNameHash(name, vol.upcase_table);
            }

            std::string utf8_name = SanitizeName(Utf16ToUtf8(name));
            if (utf8_name.empty()) {
                i += secondary_count;
                continue;
            }

            fs::path entry_rel = rel_path / utf8_name;
            std::string status = in_use ? "complete" : "deleted-complete";

            if (is_dir) {
                // Directories are structural only; the manifest lists them,
                // but the actual folder is created lazily under whichever
                // destination its first recovered file lands in.
                if (!entry_set_valid) {
                    status = in_use ? "metadata-damaged" : "deleted-metadata-damaged";
                    fprintf(stderr, "warning: directory entry '%s' failed checksum/name validation; "
                            "not descending into it (first cluster %u)\n",
                            entry_rel.generic_string().c_str(), first_cluster);
                }
                manifest.push_back({entry_rel.generic_string(), 0, 0, true, status, "", "", 0,
                                    modified_date});
                if (entry_set_valid)
                    WalkDirectory(first_cluster, no_fat_chain_flag, data_length, entry_rel, depth + 1);
            } else if (!filters.Matches(entry_rel, data_length, modified_date_key)) {
                manifest.push_back({entry_rel.generic_string(), data_length, 0, false, "skipped-filtered", "", "", 0,
                                    modified_date});
            } else {
                pending_files.push_back({first_cluster, no_fat_chain_flag, data_length, valid_data_length,
                                         entry_rel, in_use, entry_set_valid,
                                         filters.Priority(entry_rel), modified_date_key, modified_date});
            }

            i += secondary_count;
        }
    }

    // Scans the first sector of every (allocated, when the bitmap is trusted)
    // cluster for a directory signature and returns the clusters that the live
    // tree does not already cover. Reads one sector per cluster, so it costs
    // seeks rather than bandwidth.
    std::vector<uint32_t> FindLostDirectories(bool all_clusters) {
        std::vector<uint32_t> found;
        const uint32_t total = vol.boot.cluster_count;
        const size_t probe = static_cast<size_t>(std::min<uint64_t>(vol.bytes_per_sector, 4096));
        std::vector<uint8_t> sector(probe);
        std::vector<DirEntryRaw> entries(probe / sizeof(DirEntryRaw));
        uint64_t probed = 0, skipped_unallocated = 0, read_failures = 0;
        const auto start = std::chrono::steady_clock::now();
        auto last_print = start;
        const bool use_bitmap = vol.allocation_bitmap_valid && !all_clusters;
        fprintf(stderr, "searching %u clusters for lost directories (%s)\n", total,
                use_bitmap ? "allocated clusters only" : "every cluster");
        for (uint32_t cluster = 2; cluster < total + 2; ++cluster) {
            if (use_bitmap && !vol.ClusterAllocated(cluster)) { ++skipped_unallocated; continue; }
            if (directory_clusters.count(cluster)) continue;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_print > std::chrono::seconds(2)) {
                const double elapsed = std::chrono::duration<double>(now - start).count();
                const double done = static_cast<double>(cluster - 2) / total;
                const double eta = done > 0 ? elapsed / done - elapsed : 0;
                fprintf(stderr, "\r%5.1f%%  cluster %u  %llu probed  %zu found  %llu read failures  ETA %02d:%02d:%02d   ",
                        100.0 * done, cluster, static_cast<unsigned long long>(probed), found.size(),
                        static_cast<unsigned long long>(read_failures),
                        static_cast<int>(eta / 3600), static_cast<int>(eta / 60) % 60, static_cast<int>(eta) % 60);
                last_print = now;
            }
            ++probed;
            if (!vol.img.ReadAt(vol.ClusterOffset(cluster), sector.data(), probe)) { ++read_failures; continue; }
            memcpy(entries.data(), sector.data(), entries.size() * sizeof(DirEntryRaw));
            if (LooksLikeDirectoryStart(entries)) found.push_back(cluster);
        }
        fprintf(stderr, "\rlost-directory search done: %llu clusters probed, %llu unallocated skipped, "
                "%llu read failures, %zu candidate directories\n",
                static_cast<unsigned long long>(probed), static_cast<unsigned long long>(skipped_unallocated),
                static_cast<unsigned long long>(read_failures), found.size());
        return found;
    }

    // Reads one directory cluster and returns the first clusters of the
    // subdirectories it names, so nested lost directories are walked once,
    // under their parent, instead of also as separate roots.
    std::vector<uint32_t> SubdirectoryClusters(uint32_t cluster) {
        std::vector<uint32_t> children;
        std::vector<uint8_t> buf;
        if (!vol.ReadCluster(cluster, buf)) return children;
        const size_t count = buf.size() / sizeof(DirEntryRaw);
        const auto* entries = reinterpret_cast<const DirEntryRaw*>(buf.data());
        for (size_t i = 0; i + 1 < count; ++i) {
            if ((entries[i].entry_type & ~kInUseBit) != (kEntryTypeFile & ~kInUseBit)) continue;
            uint16_t attrs = 0;
            memcpy(&attrs, &entries[i].data[3], 2);
            if (!(attrs & kAttrDirectory)) continue;
            if ((entries[i + 1].entry_type & ~kInUseBit) != (kEntryTypeStreamExt & ~kInUseBit)) continue;
            uint32_t first = 0;
            memcpy(&first, &entries[i + 1].data[19], 4);
            if (first >= 2) children.push_back(first);
        }
        return children;
    }

    void WalkLostDirectories(const std::vector<uint32_t>& lost) {
        std::unordered_set<uint32_t> nested;
        for (uint32_t cluster : lost)
            for (uint32_t child : SubdirectoryClusters(cluster)) nested.insert(child);
        size_t roots = 0;
        for (uint32_t cluster : lost) {
            if (nested.count(cluster) || directory_clusters.count(cluster)) continue;
            ++roots;
            const fs::path rel = fs::path("LOST.DIR") / ("cluster_" + std::to_string(cluster));
            manifest.push_back({rel.generic_string(), 0, 0, true, "lost-directory", "", "", 0, ""});
            WalkDirectory(cluster, /*no_fat_chain=*/false, 0, rel, 1);
        }
        fprintf(stderr, "walked %zu lost directory roots (%zu nested under another lost directory)\n",
                roots, lost.size() - roots);
    }

    void RecoverPending() {
        std::stable_sort(pending_files.begin(), pending_files.end(),
            [&](const PendingFile& a, const PendingFile& b) {
                if (a.priority != b.priority) return a.priority < b.priority;
                const uint64_t a_offset = a.first_cluster >= 2 &&
                    a.first_cluster < vol.boot.cluster_count + 2
                    ? vol.ClusterOffset(a.first_cluster) : std::numeric_limits<uint64_t>::max();
                const uint64_t b_offset = b.first_cluster >= 2 &&
                    b.first_cluster < vol.boot.cluster_count + 2
                    ? vol.ClusterOffset(b.first_cluster) : std::numeric_limits<uint64_t>::max();
                return a_offset < b_offset;
            });
        if (!scan_only) {
            RecoverPendingInDiskOrder();
            return;
        }
        for (const auto& file : pending_files) {
            const size_t manifest_before = manifest.size();
            RecoverFile(file.first_cluster, file.no_fat_chain, file.data_length,
                        file.valid_data_length,
                        file.rel_path, file.in_use, file.metadata_valid);
            if (manifest.size() > manifest_before) manifest.back().modified_date = file.modified_date;
        }
    }

    void RecoverPendingInDiskOrder() {
        struct Prepared {
            PendingFile file;
            Destination* destination = nullptr;
            fs::path output;
            std::string extents;
            uint64_t damaged_bytes = 0;
            uint64_t source_bytes_written = 0;
            bool source_complete = true;
            bool deleted_allocation_conflict = false;
            bool read_error = false;
            bool write_error = false;
            bool beyond_capacity = false;
        };
        struct Task {
            uint64_t source_offset;
            uint64_t length;
            uint64_t logical_offset;
            size_t prepared_index;
            int priority;
        };

        std::vector<Prepared> prepared;
        std::vector<Task> tasks;
        for (const auto& file : pending_files) {
            const auto clusters = ClusterChain(vol, file.first_cluster,
                                               file.no_fat_chain, file.data_length);
            bool metadata_damaged = !file.metadata_valid ||
                (file.data_length != 0 && clusters.empty());
            bool live_allocation_conflict = false;
            bool deleted_allocation_conflict = false;
            bool source_complete = true;
            uint64_t damaged_bytes = 0;
            uint64_t covered = 0;
            std::string extents;
            uint64_t extent_start = 0, extent_length = 0;

            for (uint32_t cluster : clusters) {
                if (covered == file.data_length) break;
                const uint64_t logical_offset = covered;
                const uint64_t allocated_length = std::min<uint64_t>(
                    file.data_length - covered, vol.bytes_per_cluster);
                const uint64_t source_length = covered < file.valid_data_length
                    ? std::min<uint64_t>(allocated_length, file.valid_data_length - covered) : 0;
                const uint64_t source_offset = vol.ClusterOffset(cluster);
                if (vol.allocation_bitmap_valid) {
                    const bool allocated = vol.ClusterAllocated(cluster);
                    if (file.in_use && !allocated) live_allocation_conflict = true;
                    if (!file.in_use && allocated) deleted_allocation_conflict = true;
                }
                if (source_length != 0) {
                    if (extent_length != 0 && extent_start + extent_length == source_offset) {
                        extent_length += source_length;
                    } else {
                        if (extent_length != 0) {
                            if (!extents.empty()) extents += ';';
                            extents += std::to_string(extent_start) + ":" + std::to_string(extent_length);
                        }
                        extent_start = source_offset;
                        extent_length = source_length;
                    }
                    const uint64_t damaged = vol.UnrescuedBytes(source_offset, source_length);
                    damaged_bytes += damaged;
                    if (damaged != 0) source_complete = false;
                }
                covered += allocated_length;
                (void)logical_offset;
            }
            if (extent_length != 0) {
                if (!extents.empty()) extents += ';';
                extents += std::to_string(extent_start) + ":" + std::to_string(extent_length);
            }
            if (covered != file.data_length || live_allocation_conflict) metadata_damaged = true;
            if (metadata_damaged) {
                const std::string status = file.in_use ? "metadata-damaged" : "deleted-metadata-damaged";
                manifest.push_back({file.rel_path.generic_string(), file.data_length, 0, false,
                                    status, "", extents, damaged_bytes, file.modified_date});
                continue;
            }

            bool resumed = false;
            for (auto& candidate_destination : destinations) {
                const fs::path candidate = candidate_destination.root / file.rel_path;
                const auto verified_hash = VerifyExistingOutput(candidate, file.data_length);
                if (!verified_hash) continue;
                std::string status = file.in_use ? "verified-existing" : "deleted-verified-existing";
                manifest.push_back({file.rel_path.generic_string(), file.data_length,
                                    file.data_length, false, status, candidate_destination.label,
                                    extents, damaged_bytes, file.modified_date, *verified_hash});
                resumed = true;
                break;
            }
            if (resumed) continue;

            Destination* destination = PickDestination(file.data_length);
            if (!destination) {
                manifest.push_back({file.rel_path.generic_string(), file.data_length, 0, false,
                                    "skipped-no-space", "", extents, damaged_bytes, file.modified_date});
                continue;
            }
            const fs::path output = UniqueOutputPath(*destination, file.rel_path);
            std::error_code ec;
            recovery::CreateDirectoriesWin32(output.parent_path(), ec);
            if (ec) {
                manifest.push_back({file.rel_path.generic_string(), file.data_length, 0, false,
                                    "write-error", destination->label, extents, damaged_bytes, file.modified_date});
                continue;
            }
            std::ofstream create(output, std::ios::binary | std::ios::trunc);
            if (!create.is_open()) {
                manifest.push_back({file.rel_path.generic_string(), file.data_length, 0, false,
                                    "write-error", destination->label, extents, damaged_bytes, file.modified_date});
                continue;
            }
            if (file.data_length != 0) {
                create.seekp(static_cast<std::streamoff>(file.data_length - 1));
                create.put('\0');
            }
            create.flush();
            create.close();
            if (!create.good()) {
                manifest.push_back({file.rel_path.generic_string(), file.data_length, 0, false,
                                    "write-error", destination->label, extents, damaged_bytes, file.modified_date});
                continue;
            }

            const size_t prepared_index = prepared.size();
            prepared.push_back({file, destination, output, extents, damaged_bytes, 0,
                                source_complete, deleted_allocation_conflict, false, false});
            covered = 0;
            for (uint32_t cluster : clusters) {
                if (covered == file.data_length) break;
                const uint64_t allocated_length = std::min<uint64_t>(
                    file.data_length - covered, vol.bytes_per_cluster);
                const uint64_t source_length = covered < file.valid_data_length
                    ? std::min<uint64_t>(allocated_length, file.valid_data_length - covered) : 0;
                if (source_length != 0) {
                    tasks.push_back({vol.ClusterOffset(cluster), source_length, covered,
                                     prepared_index, file.priority});
                }
                covered += allocated_length;
            }
        }

        std::stable_sort(tasks.begin(), tasks.end(), [](const Task& a, const Task& b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            if (a.source_offset != b.source_offset) return a.source_offset < b.source_offset;
            return a.length < b.length;
        });
        planned_extent_tasks = tasks.size();
        uint64_t planned_bytes = 0;
        for (const auto& t : tasks) planned_bytes += t.length;
        printf("recovering %zu files, %.2f GB across %zu extents in physical order\n",
               prepared.size(), planned_bytes / (1024.0 * 1024.0 * 1024.0), tasks.size());
        fflush(stdout);
        const auto start_time = std::chrono::steady_clock::now();
        auto last_progress_print = start_time;
        uint64_t processed_bytes = 0;
        uint64_t read_failures = 0;

        std::vector<uint8_t> buffer;
        for (size_t i = 0; i < tasks.size();) {
            const uint64_t source_offset = tasks[i].source_offset;
            const uint64_t length = tasks[i].length;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_progress_print > std::chrono::seconds(2)) {
                const double elapsed = std::chrono::duration<double>(now - start_time).count();
                const double rate = elapsed > 0 ? processed_bytes / elapsed : 0.0;
                const double remaining = planned_bytes > processed_bytes
                    ? planned_bytes - processed_bytes : 0.0;
                const unsigned long long eta = rate > 0
                    ? static_cast<unsigned long long>(remaining / rate) : 0;
                printf("\r%5.1f%%  %.2f/%.2f GB  %.1f MB/s  ETA %02llu:%02llu:%02llu  "
                       "read failures %llu  extent %zu/%zu at %.2f GB     ",
                       planned_bytes ? 100.0 * processed_bytes / planned_bytes : 100.0,
                       processed_bytes / (1024.0 * 1024.0 * 1024.0),
                       planned_bytes / (1024.0 * 1024.0 * 1024.0),
                       rate / (1024.0 * 1024.0), eta / 3600, (eta / 60) % 60, eta % 60,
                       static_cast<unsigned long long>(read_failures), i, tasks.size(),
                       source_offset / (1024.0 * 1024.0 * 1024.0));
                fflush(stdout);
                last_progress_print = now;
            }
            buffer.resize(static_cast<size_t>(length));
            const bool beyond_capacity = vol.real_capacity != 0 && source_offset >= vol.real_capacity;
            const bool read_ok = !beyond_capacity &&
                vol.img.ReadAt(source_offset, buffer.data(), buffer.size());
            size_t group_end = i + 1;
            while (group_end < tasks.size() && tasks[group_end].priority == tasks[i].priority &&
                   tasks[group_end].source_offset == source_offset && tasks[group_end].length == length) {
                ++group_end;
            }
            ++unique_extent_reads;
            deduplicated_extent_tasks += group_end - i - 1;
            if (read_ok) source_data_bytes_read += length;
            else if (!beyond_capacity) ++read_failures;
            for (size_t task_index = i; task_index < group_end; ++task_index)
                processed_bytes += tasks[task_index].length;
            for (size_t task_index = i; task_index < group_end; ++task_index) {
                Prepared& item = prepared[tasks[task_index].prepared_index];
                if (!read_ok) {
                    item.read_error = true;
                    item.source_complete = false;
                    if (beyond_capacity) item.beyond_capacity = true;
                    continue;
                }
                std::fstream output(item.output, std::ios::binary | std::ios::in | std::ios::out);
                if (!output.is_open()) {
                    item.write_error = true;
                    continue;
                }
                output.seekp(static_cast<std::streamoff>(tasks[task_index].logical_offset));
                output.write(reinterpret_cast<const char*>(buffer.data()),
                             static_cast<std::streamsize>(buffer.size()));
                output.flush();
                output.close();
                if (!output.good()) item.write_error = true;
                else item.source_bytes_written += length;
            }
            i = group_end;
        }
        if (!tasks.empty()) {
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_time).count();
            printf("\rdata pass finished: %.2f GB in %.0f s, %llu extent read failures; verifying...        \n",
                   processed_bytes / (1024.0 * 1024.0 * 1024.0), elapsed,
                   static_cast<unsigned long long>(read_failures));
            fflush(stdout);
        }

        for (auto& item : prepared) {
            const uint64_t initialized_zeros = item.file.data_length - item.file.valid_data_length;
            uint64_t recovered_bytes = item.source_bytes_written + initialized_zeros;
            item.destination->used += item.file.data_length;
            std::string status;
            if (item.write_error) status = "write-error";
            else if (item.read_error && item.source_bytes_written == 0) {
                status = item.beyond_capacity ? "beyond-device-capacity" : "unreadable";
                // Nothing real was written; do not leave a zero-filled file
                // that looks like a recovered one.
                std::error_code remove_ec;
                fs::remove(item.output, remove_ec);
                recovered_bytes = 0;
            }
            else if (item.read_error || !item.source_complete || item.deleted_allocation_conflict)
                status = "partial";
            else status = "complete";
            if (!item.file.in_use) status = "deleted-" + status;
            std::string hash;
            if (status == "complete" || status == "deleted-complete") {
                const auto calculated = recovery::Sha256File(item.output);
                if (calculated && WriteVerification(item.output, item.file.data_length, *calculated)) {
                    hash = *calculated;
                } else {
                    status = item.file.in_use ? "complete-unverified" : "deleted-complete-unverified";
                }
            }
            manifest.push_back({item.file.rel_path.generic_string(), item.file.data_length,
                                recovered_bytes, false, status, item.destination->label,
                                item.extents, item.damaged_bytes, item.file.modified_date, hash});
        }
    }

    void RecoverFile(uint32_t first_cluster, bool no_fat_chain, uint64_t data_length,
                      uint64_t valid_data_length,
                      const fs::path& rel_path, bool in_use, bool entry_set_valid) {
        auto clusters = ClusterChain(vol, first_cluster, no_fat_chain, data_length);
        const bool chain_valid_initially = data_length == 0 || !clusters.empty();
        bool source_complete = chain_valid_initially;
        bool live_allocation_conflict = false;
        bool deleted_allocation_conflict = false;
        uint64_t covered = 0;
        uint64_t damaged_bytes = 0;
        uint64_t beyond_capacity_bytes = 0;
        std::string extents;
        uint64_t extent_start = 0, extent_length = 0;
        for (uint32_t c : clusters) {
            const uint64_t take = std::min<uint64_t>(data_length - covered, vol.bytes_per_cluster);
            const uint64_t source_take = covered < valid_data_length
                ? std::min<uint64_t>(take, valid_data_length - covered) : 0;
            const uint64_t offset = vol.ClusterOffset(c);
            if (vol.allocation_bitmap_valid) {
                const bool allocated = vol.ClusterAllocated(c);
                if (in_use && !allocated) live_allocation_conflict = true;
                if (!in_use && allocated) deleted_allocation_conflict = true;
            }
            if (source_take != 0) {
                if (extent_length != 0 && extent_start + extent_length == offset) {
                    extent_length += source_take;
                } else {
                    if (extent_length != 0) {
                        if (!extents.empty()) extents += ';';
                        extents += std::to_string(extent_start) + ":" + std::to_string(extent_length);
                    }
                    extent_start = offset;
                    extent_length = source_take;
                }
            }
            const uint64_t damaged = vol.UnrescuedBytes(offset, source_take);
            beyond_capacity_bytes += vol.BeyondCapacityBytes(offset, source_take);
            if (damaged != 0) {
                source_complete = false;
                damaged_bytes += damaged;
            }
            covered += take;
            if (covered == data_length) break;
        }
        if (extent_length != 0) {
            if (!extents.empty()) extents += ';';
            extents += std::to_string(extent_start) + ":" + std::to_string(extent_length);
        }
        if (covered != data_length) source_complete = false;

        const bool metadata_damaged = !entry_set_valid || !chain_valid_initially ||
            covered != data_length || live_allocation_conflict;
        const bool source_health_known = vol.rescue_map != nullptr || !vol.img.IsDevice() ||
            vol.real_capacity != 0;
        auto SourceStatus = [&]() {
            if (metadata_damaged) return std::string("metadata-damaged");
            if (valid_data_length != 0 && damaged_bytes == valid_data_length)
                return std::string(beyond_capacity_bytes == valid_data_length ? "beyond-device-capacity" : "unreadable");
            if (!source_complete || deleted_allocation_conflict) return std::string("partial");
            return std::string("complete");
        };

        if (scan_only) {
            std::string status = "planned-" +
                ((!metadata_damaged && !source_health_known) ? std::string("unknown") : SourceStatus());
            if (!in_use) status = "deleted-" + status;
            manifest.push_back({rel_path.generic_string(), data_length, 0, false, status, "", extents, damaged_bytes});
            return;
        }

        if (metadata_damaged) {
            std::string status = in_use ? "metadata-damaged" : "deleted-metadata-damaged";
            manifest.push_back({rel_path.generic_string(), data_length, 0, false, status, "", extents, damaged_bytes});
            return;
        }

        Destination* dest = PickDestination(data_length);
        if (!dest) {
            manifest.push_back({rel_path.generic_string(), data_length, 0, false, "skipped-no-space", "", extents, damaged_bytes});
            return;
        }

        fs::path out_path = UniqueOutputPath(*dest, rel_path);
        std::error_code ec;
        recovery::CreateDirectoriesWin32(out_path.parent_path(), ec);
        if (ec) {
            manifest.push_back({rel_path.generic_string(), data_length, 0, false, "write-error", dest->label, extents, damaged_bytes});
            return;
        }
        std::error_code source_ec;
        if (fs::equivalent(out_path, source_path, source_ec) && !source_ec) {
            manifest.push_back({rel_path.generic_string(), data_length, 0, false, "source-alias", dest->label, extents, damaged_bytes});
            return;
        }

        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            manifest.push_back({rel_path.generic_string(), data_length, 0, false, "write-error", dest->label, extents, damaged_bytes});
            return;
        }
        uint64_t remaining = data_length;
        std::vector<uint8_t> buf;
        std::vector<uint8_t> zeros(static_cast<size_t>(
            std::min<uint64_t>(vol.bytes_per_cluster, 1024 * 1024)), 0);
        bool ok = source_complete;
        bool write_error = false;
        bool read_error = false;
        for (uint32_t c : clusters) {
            if (remaining == 0) break;
            const uint64_t logical_offset = data_length - remaining;
            const uint64_t take = std::min<uint64_t>(remaining, vol.bytes_per_cluster);
            const uint64_t valid_take = logical_offset < valid_data_length
                ? std::min<uint64_t>(take, valid_data_length - logical_offset) : 0;
            bool source_fully_rescued = true;
            if (valid_take != 0) {
                if (!vol.ReadCluster(c, buf, &source_fully_rescued)) {
                    ok = false;
                    read_error = true;
                    break;
                }
                if (!source_fully_rescued) ok = false;
                out.write(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(valid_take));
                if (!out.good()) { ok = false; write_error = true; break; }
            }
            uint64_t zero_remaining = take - valid_take;
            while (zero_remaining != 0) {
                const size_t zero_take = static_cast<size_t>(
                    std::min<uint64_t>(zero_remaining, zeros.size()));
                out.write(reinterpret_cast<char*>(zeros.data()), static_cast<std::streamsize>(zero_take));
                if (!out.good()) { ok = false; write_error = true; break; }
                zero_remaining -= zero_take;
            }
            if (write_error) break;
            remaining -= take;
        }
        out.flush();
        out.close();
        if (!out.good()) { ok = false; write_error = true; }
        uint64_t written = data_length - remaining;
        dest->used += written;
        std::string status = write_error ? "write-error" :
            (read_error ? (written == 0 ? "unreadable" : "partial") :
             ((ok && remaining == 0) ? "complete" : SourceStatus()));
        if (!in_use) status = "deleted-" + status;
        manifest.push_back({rel_path.generic_string(), data_length, written, false, status, dest->label, extents, damaged_bytes});
    }
};

// Parses sizes like "2TB", "500GB", "128MB", "1024" (bytes). Binary units
// (1TB = 1024^4). Returns 0 on "0"/"unlimited" (meaning: no cap).
uint64_t ParseSize(const std::string& raw) {
    std::string s = raw;
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (s == "0" || s == "UNLIMITED") return 0;

    size_t split = s.find_first_not_of("0123456789.");
    double value;
    try {
        size_t consumed = 0;
        value = std::stod(s.substr(0, split), &consumed);
        if (consumed != s.substr(0, split).size()) throw std::invalid_argument("trailing junk");
    } catch (const std::exception&) {
        fprintf(stderr, "invalid size '%s'\n", raw.c_str());
        exit(1);
    }
    std::string unit = split == std::string::npos ? "" : s.substr(split);
    uint64_t mult = 1;
    if (unit == "K" || unit == "KB") mult = 1024ull;
    else if (unit == "M" || unit == "MB") mult = 1024ull * 1024;
    else if (unit == "G" || unit == "GB") mult = 1024ull * 1024 * 1024;
    else if (unit == "T" || unit == "TB") mult = 1024ull * 1024 * 1024 * 1024;
    else if (!unit.empty()) {
        fprintf(stderr, "unrecognized size unit '%s' in '%s'\n", unit.c_str(), raw.c_str());
        exit(1);
    }
    return static_cast<uint64_t>(value * mult);
}

uint32_t ParseDate(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        throw std::invalid_argument("date must be YYYY-MM-DD");
    const unsigned year = static_cast<unsigned>(std::stoul(value.substr(0, 4)));
    const unsigned month = static_cast<unsigned>(std::stoul(value.substr(5, 2)));
    const unsigned day = static_cast<unsigned>(std::stoul(value.substr(8, 2)));
    if (year < 1980 || year > 2107 || month < 1 || month > 12 || day < 1 || day > 31)
        throw std::invalid_argument("date is outside the exFAT range");
    return year * 10000 + month * 100 + day;
}

std::vector<std::string> SplitCsv(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        std::string part = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty()) out.push_back(Filters::Lower(part));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

bool LoadSelectedPaths(const fs::path& path, Filters& filters) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    if (!std::getline(in, line) || line.rfind("selected,priority,path,", 0) != 0) return false;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        const size_t first_comma = line.find(',');
        const size_t second_comma = first_comma == std::string::npos
            ? std::string::npos : line.find(',', first_comma + 1);
        if (first_comma == std::string::npos || second_comma == std::string::npos) return false;
        if (line.substr(0, first_comma) != "1") continue;
        int priority = 100;
        try { priority = std::stoi(line.substr(first_comma + 1, second_comma - first_comma - 1)); }
        catch (...) { return false; }
        size_t pos = second_comma + 1;
        if (pos >= line.size() || line[pos] != '"') return false;
        ++pos;
        std::string value;
        bool closed = false;
        while (pos < line.size()) {
            if (line[pos] != '"') {
                value.push_back(line[pos++]);
            } else if (pos + 1 < line.size() && line[pos + 1] == '"') {
                value.push_back('"');
                pos += 2;
            } else {
                closed = true;
                break;
            }
        }
        if (!closed) return false;
        const std::string normalized = Filters::Lower(value);
        filters.exact_paths.insert(normalized);
        filters.priorities[normalized] = priority;
    }
    return !in.bad();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: exfat_recovery.exe <source> <output_dir> [options]\n"
                "       exfat_recovery.exe <source> --dest <dir1> <size1> [--dest <dir2> <size2> ...] [options]\n"
                "       exfat_recovery.exe <source> --scan <plan.csv> [options]\n"
                "\n"
                "  <output_dir>              single unlimited destination (shorthand for one --dest with size 0)\n"
                "  --dest <dir> <size>       add an output destination with a capacity cap, e.g. --dest D:\\r1 2TB\n"
                "                            repeatable; destinations fill in the order given, size 0/unlimited = no cap\n"
                "  --include-deleted         also recover entries whose in-use flag is cleared\n"
                "  --only-ext a,b,c          only recover files with these extensions (no dot, comma separated)\n"
                "  --path-contains a,b       only recover files whose path contains one of these substrings\n"
                "  --min-size size           minimum logical file size (e.g. 1MB)\n"
                "  --max-size size           maximum logical file size (e.g. 50GB)\n"
                "  --modified-after date     include dates on/after YYYY-MM-DD\n"
                "  --modified-before date    include dates on/before YYYY-MM-DD\n"
                "  --offset bytes            exFAT volume byte offset (default: auto-detect GPT/MBR)\n"
                "  --source-size bytes       raw-device capacity override if its USB bridge rejects size IOCTLs\n"
                "  --map path                imager rescue map (default: <image>.map if present)\n"
                "  --scan plan.csv           metadata-only inventory; writes no recovered file data\n"
                "  --from-plan plan.csv      recover only rows whose selected column is 1\n"
                "  --find-lost-dirs out.txt  probe every allocated cluster for a directory whose parent entry\n"
                "                            was wiped; writes the cluster list (add --all-clusters to ignore\n"
                "                            the allocation bitmap)\n"
                "  --lost-dirs out.txt       also walk those clusters as LOST.DIR/cluster_N roots in scan/recover\n"
                "  --real-capacity size      fake-capacity device: bytes at/after this source offset were never\n"
                "                            stored; such files are reported beyond-device-capacity, not written\n"
                "\n"
                "manifest.csv is written into the first destination and lists every entry found, including\n"
                "ones skipped by a filter (status 'skipped-filtered') or because every destination was full\n"
                "(status 'skipped-no-space') -- rerun with more/bigger --dest entries to pick those up.\n");
        return 1;
    }
    std::string image_path = argv[1];

    std::vector<Destination> destinations;
    Filters filters;
    bool include_deleted = false;
    std::optional<uint64_t> volume_offset;
    std::string map_path;
    bool no_map = false;
    fs::path scan_path;
    bool scan_only = false;
    fs::path from_plan;
    uint64_t source_size_override = 0;

    fs::path find_lost_path;
    fs::path lost_dirs_path;
    bool all_clusters = false;
    uint64_t real_capacity = 0;

    int i = 2;
    if (std::string(argv[i]) == "--scan" && i + 1 < argc) {
        scan_only = true;
        scan_path = argv[i + 1];
        i += 2;
    } else if (std::string(argv[i]) == "--find-lost-dirs" && i + 1 < argc) {
        scan_only = true;
        find_lost_path = argv[i + 1];
        i += 2;
    } else if (std::string(argv[i]) != "--dest") {
        // Shorthand: exfat-parser.exe image.img output_dir [options]
        destinations.push_back({argv[i], fs::path(argv[i]), 0, 0});
        ++i;
    }
    for (; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--include-deleted") {
            include_deleted = true;
        } else if (arg == "--dest" && i + 2 < argc) {
            std::string dir = argv[++i];
            uint64_t cap = ParseSize(argv[++i]);
            destinations.push_back({dir, fs::path(dir), cap, 0});
        } else if (arg == "--only-ext" && i + 1 < argc) {
            for (auto& e : SplitCsv(argv[++i])) filters.only_ext.push_back(e);
        } else if (arg == "--path-contains" && i + 1 < argc) {
            for (auto& e : SplitCsv(argv[++i])) filters.path_contains.push_back(e);
        } else if (arg == "--min-size" && i + 1 < argc) {
            filters.min_size = ParseSize(argv[++i]);
        } else if (arg == "--max-size" && i + 1 < argc) {
            filters.max_size = ParseSize(argv[++i]);
        } else if (arg == "--modified-after" && i + 1 < argc) {
            try { filters.modified_after = ParseDate(argv[++i]); }
            catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return 1; }
        } else if (arg == "--modified-before" && i + 1 < argc) {
            try { filters.modified_before = ParseDate(argv[++i]); }
            catch (const std::exception& e) { fprintf(stderr, "%s\n", e.what()); return 1; }
        } else if (arg == "--offset" && i + 1 < argc) {
            volume_offset = std::stoull(argv[++i]);
        } else if (arg == "--no-map") {
            no_map = true;
        } else if (arg == "--source-size" && i + 1 < argc) {
            source_size_override = std::stoull(argv[++i]);
        } else if (arg == "--map" && i + 1 < argc) {
            map_path = argv[++i];
        } else if (arg == "--from-plan" && i + 1 < argc) {
            from_plan = argv[++i];
        } else if (arg == "--lost-dirs" && i + 1 < argc) {
            lost_dirs_path = argv[++i];
        } else if (arg == "--all-clusters") {
            all_clusters = true;
        } else if (arg == "--real-capacity" && i + 1 < argc) {
            real_capacity = ParseSize(argv[++i]);
        } else {
            fprintf(stderr, "unrecognized argument: %s\n", arg.c_str());
            return 1;
        }
    }
    if (destinations.empty() && !scan_only) {
        fprintf(stderr, "no output destination given\n");
        return 1;
    }
    if (!from_plan.empty() && !LoadSelectedPaths(from_plan, filters)) {
        fprintf(stderr, "cannot load selected rows from plan %s\n", from_plan.string().c_str());
        return 1;
    }
    if (!from_plan.empty()) filters.exact_paths_enabled = true;

    Volume vol;
    if (!OpenVolume(image_path, vol, volume_offset, source_size_override)) return 1;
    vol.real_capacity = real_capacity;
    if (real_capacity != 0) {
        printf("treating every byte at or beyond offset %llu (%.2f GB) as never stored by the device\n",
               static_cast<unsigned long long>(real_capacity), real_capacity / (1024.0 * 1024.0 * 1024.0));
    }

    if (!no_map && map_path.empty() && fs::exists(image_path + ".map")) map_path = image_path + ".map";
    std::unique_ptr<recovery::RescueMap> rescue_map;
    if (!map_path.empty()) {
        rescue_map = std::make_unique<recovery::RescueMap>(vol.image_size);
        if (!rescue_map->Load(map_path)) {
            fprintf(stderr, "invalid rescue map: %s\n", map_path.c_str());
            return 1;
        }
        vol.rescue_map = rescue_map.get();
    }

    for (auto& d : destinations) {
        std::error_code ec;
        recovery::CreateDirectoriesWin32(d.root, ec);
        if (ec || !fs::is_directory(d.root)) {
            fprintf(stderr, "cannot create output directory %s\n", d.label.c_str());
            return 1;
        }
    }

    const fs::path absolute_image_path = recovery::FullPathWin32(image_path);
    Recovery recovery{vol, destinations, filters, include_deleted, scan_only,
                      absolute_image_path, {}, {}, {}};
    printf("reading directory tree...\n");
    fflush(stdout);
    recovery.WalkDirectory(vol.boot.first_cluster_of_root_dir, /*no_fat_chain=*/false, 0, "", 0);
    if (!find_lost_path.empty()) {
        const auto lost = recovery.FindLostDirectories(all_clusters);
        std::ofstream out(find_lost_path, std::ios::trunc);
        if (!out.is_open()) {
            fprintf(stderr, "cannot write %s\n", find_lost_path.string().c_str());
            return 1;
        }
        out << "# exFAT lost directory clusters found by signature in " << image_path << "\n";
        for (uint32_t cluster : lost) out << cluster << "\n";
        out.flush();
        out.close();
        if (!out.good()) {
            fprintf(stderr, "failed while writing %s\n", find_lost_path.string().c_str());
            return 1;
        }
        printf("%zu lost directory clusters written to %s; pass it back with --lost-dirs\n",
               lost.size(), find_lost_path.string().c_str());
        return 0;
    }
    if (!lost_dirs_path.empty()) {
        std::ifstream in(lost_dirs_path);
        if (!in.is_open()) {
            fprintf(stderr, "cannot read %s\n", lost_dirs_path.string().c_str());
            return 1;
        }
        std::vector<uint32_t> lost;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            try { lost.push_back(static_cast<uint32_t>(std::stoul(line))); }
            catch (const std::exception&) {
                fprintf(stderr, "bad cluster number in %s: %s\n", lost_dirs_path.string().c_str(), line.c_str());
                return 1;
            }
        }
        recovery.WalkLostDirectories(lost);
    }
    {
        uint64_t selected_bytes = 0;
        for (const auto& f : recovery.pending_files) selected_bytes += f.data_length;
        printf("directory tree read: %zu files match (%.2f GB)%s\n", recovery.pending_files.size(),
               selected_bytes / (1024.0 * 1024.0 * 1024.0),
               scan_only ? "; writing plan only, no file data" : "");
        fflush(stdout);
    }
    recovery.RecoverPending();

    fs::path manifest_path = scan_only ? scan_path : destinations.front().root / "manifest.csv";
    if (!manifest_path.parent_path().empty()) {
        std::error_code ec;
        recovery::CreateDirectoriesWin32(manifest_path.parent_path(), ec);
        if (ec) {
            fprintf(stderr, "cannot create manifest directory\n");
            return 1;
        }
    }
    std::ofstream manifest(manifest_path, std::ios::trunc);
    if (!manifest.is_open()) {
        fprintf(stderr, "cannot write manifest %s\n", manifest_path.string().c_str());
        return 1;
    }
    manifest << "selected,priority,path,logical_size,written_size,status,destination,source_extents,damaged_bytes,modified_date,sha256,confidence\n";
    size_t complete = 0, partial = 0, skipped = 0;
    for (const auto& e : recovery.manifest) {
        manifest << (e.is_directory ? 0 : 1) << ',' << filters.Priority(e.path) << ',' << CsvField(e.path) << ','
                 << e.logical_size << ',' << e.written_size << ','
                 << e.status << ',' << CsvField(e.destination) << ',' << CsvField(e.source_extents)
                 << ',' << e.damaged_bytes << ',' << CsvField(e.modified_date) << ','
                 << CsvField(e.sha256) << ',' << MetadataConfidence(e.status, vol) << '\n';
        if (e.status.find("skipped") != std::string::npos) ++skipped;
        else if (e.status.find("complete") != std::string::npos) ++complete;
        else if (e.status.find("partial") != std::string::npos) ++partial;
    }
    manifest.flush();
    manifest.close();
    if (!manifest.good()) {
        fprintf(stderr, "failed while writing manifest %s\n", manifest_path.string().c_str());
        return 1;
    }

    const fs::path report_path = scan_only
        ? fs::path(manifest_path.string() + ".report.json")
        : destinations.front().root / "recovery_report.json";
    std::ofstream report(report_path, std::ios::trunc);
    if (!report.is_open()) {
        fprintf(stderr, "cannot write recovery report %s\n", report_path.string().c_str());
        return 1;
    }
    uint64_t logical_bytes = 0, written_bytes = 0, damaged_bytes = 0;
    for (const auto& entry : recovery.manifest) {
        if (entry.is_directory) continue;
        logical_bytes += entry.logical_size;
        written_bytes += entry.written_size;
        damaged_bytes += entry.damaged_bytes;
    }
    report << "{\n"
           << "  \"source_size\": " << vol.image_size << ",\n"
           << "  \"volume_offset\": " << vol.volume_offset << ",\n"
           << "  \"files\": " << recovery.manifest.size() << ",\n"
           << "  \"logical_bytes\": " << logical_bytes << ",\n"
           << "  \"written_bytes\": " << written_bytes << ",\n"
           << "  \"damaged_bytes\": " << damaged_bytes << ",\n"
           << "  \"planned_extent_tasks\": " << recovery.planned_extent_tasks << ",\n"
           << "  \"unique_extent_reads\": " << recovery.unique_extent_reads << ",\n"
           << "  \"deduplicated_extent_tasks\": " << recovery.deduplicated_extent_tasks << ",\n"
           << "  \"source_data_bytes_read\": " << recovery.source_data_bytes_read << "\n"
           << "}\n";
    report.flush();
    report.close();
    if (!report.good()) {
        fprintf(stderr, "failed while writing recovery report\n");
        return 1;
    }

    printf("recovered %zu entries (%zu complete, %zu partial, %zu skipped). manifest: %s\n",
           recovery.manifest.size(), complete, partial, skipped, manifest_path.string().c_str());
    for (const auto& d : destinations) {
        printf("  %s: %.2f GB used%s\n", d.label.c_str(), d.used / (1024.0 * 1024.0 * 1024.0),
               d.capacity == 0 ? " (unlimited)" : "");
    }
    return 0;
}

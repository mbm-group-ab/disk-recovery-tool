// imager: ddrescue-style raw disk imager for Windows.
//
// Reads a source device (physical drive or existing image) sector-by-sector
// into a destination image file, in large aligned chunks for speed. When a
// chunk read fails, it backs off to smaller chunks and finally single
// sectors so a handful of bad sectors don't stall the whole run; sectors
// that still fail are recorded (not zero-filled) so a later pass with a
// different tool/strategy can retry just those.
//
// Progress is checkpointed to a mapfile every few chunks, so a run killed
// partway through (e.g. because the drive is flaky and hangs) can resume
// with --resume instead of re-reading everything already rescued.
//
// Usage:
//   imager.exe \\.\PhysicalDrive2 D:\rescue\disk.img [--resume] [--sector 512]

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "chunked_image_writer.h"
#include "rescue_map.h"
#include "win32_paths.h"

namespace {

constexpr uint64_t kDefaultChunk = 8ull * 1024 * 1024;  // 8 MB
constexpr uint64_t kMinRetryChunk = 64ull * 1024;        // shrink to this before going sector-by-sector
constexpr int kMaxRetriesPerChunk = 2;
constexpr uint64_t kCheckpointEveryBytes = 512ull * 1024 * 1024;  // save map every 512MB progress
constexpr int kExitDestinationLow = 3;  // stopped cleanly because --min-dest-free was reached
constexpr int kExitDriveUnresponsive = 4;  // stopped cleanly: --max-consecutive-failures reached

// Directory whose volume holds the destination image (or the chunk index).
std::wstring DestinationDirectory(const std::wstring& dest_path) {
    const std::filesystem::path absolute = recovery::FullPathWin32(dest_path);
    std::filesystem::path dir = absolute.parent_path();
    if (dir.empty()) dir = absolute.root_path();
    return dir.wstring();
}

void FailWin32(const char* what) {
    DWORD err = GetLastError();
    fprintf(stderr, "%s failed, error %lu\n", what, err);
    exit(1);
}

HANDLE OpenSourceForRawRead(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING,
                            FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN |
                                FILE_FLAG_OVERLAPPED,
                            nullptr);
    if (h == INVALID_HANDLE_VALUE) FailWin32("CreateFile(source)");
    return h;
}

#ifndef RECOVERY_REGULAR_FILES_ONLY
bool DeviceIoControlSync(HANDLE handle, DWORD code, void* input, DWORD input_size,
                         void* output, DWORD output_size, DWORD* returned) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return false;
    BOOL ok = DeviceIoControl(handle, code, input, input_size, output, output_size,
                              returned, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = WaitForSingleObject(overlapped.hEvent, 5000) == WAIT_OBJECT_0 &&
             GetOverlappedResult(handle, &overlapped, returned, FALSE);
    }
    CloseHandle(overlapped.hEvent);
    return ok != FALSE;
}
#endif

uint64_t GetSourceSize(HANDLE h) {
    // Try as a physical/logical device first.
#ifndef RECOVERY_REGULAR_FILES_ONLY
    GET_LENGTH_INFORMATION len_info{};
    DWORD bytes_returned = 0;
    if (DeviceIoControlSync(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len_info,
                            sizeof(len_info), &bytes_returned)) {
        return static_cast<uint64_t>(len_info.Length.QuadPart);
    }
#endif
    // Fall back to a plain file size.
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) FailWin32("GetFileSizeEx(source)");
    return static_cast<uint64_t>(size.QuadPart);
}

HANDLE OpenDestImage(const std::wstring& path, uint64_t total_size, bool resume) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr, resume ? OPEN_EXISTING : CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) FailWin32("CreateFile(dest)");

    // Mark sparse so unwritten (bad-sector) regions don't consume disk space.
#ifndef RECOVERY_REGULAR_FILES_ONLY
    DWORD bytes_returned = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes_returned, nullptr);
#endif

    LARGE_INTEGER size;
    size.QuadPart = static_cast<LONGLONG>(total_size);
    if (!SetFilePointerEx(h, size, nullptr, FILE_BEGIN)) FailWin32("SetFilePointerEx(dest)");
    if (!SetEndOfFile(h)) FailWin32("SetEndOfFile(dest)");

    return h;
}

uint32_t GetLogicalSectorSize(HANDLE h) {
#ifndef RECOVERY_REGULAR_FILES_ONLY
    DISK_GEOMETRY_EX geometry{};
    DWORD bytes_returned = 0;
    if (DeviceIoControlSync(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
                            &geometry, sizeof(geometry), &bytes_returned) &&
        geometry.Geometry.BytesPerSector >= 512) {
        return geometry.Geometry.BytesPerSector;
    }
#else
    (void)h;
#endif
    return 512;
}

uint32_t GetPhysicalSectorSize(HANDLE h, uint32_t logical_sector_size) {
#ifndef RECOVERY_REGULAR_FILES_ONLY
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageAccessAlignmentProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{};
    DWORD returned = 0;
    if (DeviceIoControlSync(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                            &alignment, sizeof(alignment), &returned) &&
        alignment.BytesPerPhysicalSector >= logical_sector_size) {
        return alignment.BytesPerPhysicalSector;
    }
#else
    (void)h;
#endif
    return logical_sector_size;
}

std::wstring NormalizedPath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::transform(path.begin(), path.end(), path.begin(), towlower);
    return path;
}

std::string WideToUtf8(const std::wstring& value) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string output(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        output.data(), length, nullptr, nullptr);
    return output;
}

bool ReadAt(HANDLE h, uint64_t offset, void* buf, uint32_t size, uint32_t timeout_ms,
            uint32_t* bytes_read, DWORD* read_error) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        *read_error = GetLastError();
        return false;
    }
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, size, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms == 0 ? INFINITE : timeout_ms);
        if (wait == WAIT_TIMEOUT) {
            CancelIoEx(h, &ov);
            WaitForSingleObject(ov.hEvent, INFINITE);
            CloseHandle(ov.hEvent);
            *read_error = ERROR_TIMEOUT;
            return false;
        }
        ok = wait == WAIT_OBJECT_0 && GetOverlappedResult(h, &ov, &got, FALSE);
    }
    if (!ok) {
        const DWORD error = GetLastError();
        CloseHandle(ov.hEvent);
        if (error != ERROR_HANDLE_EOF) {
            *read_error = error;
            return false;
        }
    } else {
        CloseHandle(ov.hEvent);
    }
    *bytes_read = got;
    *read_error = ERROR_SUCCESS;
    return true;
}

bool WriteAt(HANDLE h, uint64_t offset, const void* buf, uint32_t size) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD written = 0;
    if (!WriteFile(h, buf, size, &written, &ov)) return false;
    return written == size;
}

bool ParseSize(const std::wstring& text, uint64_t& value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long long base = wcstoull(text.c_str(), &end, 10);
    if (end == text.c_str()) return false;
    uint64_t multiplier = 1;
    std::wstring suffix(end);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), towlower);
    if (suffix.empty() || suffix == L"b") multiplier = 1;
    else if (suffix == L"k" || suffix == L"kb" || suffix == L"kib") multiplier = 1024ull;
    else if (suffix == L"m" || suffix == L"mb" || suffix == L"mib") multiplier = 1024ull * 1024;
    else if (suffix == L"g" || suffix == L"gb" || suffix == L"gib") multiplier = 1024ull * 1024 * 1024;
    else if (suffix == L"t" || suffix == L"tb" || suffix == L"tib") multiplier = 1024ull * 1024 * 1024 * 1024;
    else return false;
    if (base == 0 || base > UINT64_MAX / multiplier) return false;
    value = static_cast<uint64_t>(base) * multiplier;
    return true;
}

std::string JsonString(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') output << '\\' << static_cast<char>(c);
        else if (c == '\n') output << "\\n";
        else if (c == '\r') output << "\\r";
        else if (c == '\t') output << "\\t";
        else if (c < 0x20) output << "\\u" << std::hex << std::setw(4)
                                  << std::setfill('0') << static_cast<unsigned>(c);
        else output << static_cast<char>(c);
    }
    output << '"';
    return output.str();
}

bool WriteProgressReport(const std::filesystem::path& report_path,
                         const recovery::RescueMap& map,
                         const std::string& source, const std::string& destination,
                         uint32_t sector_size, bool reverse, int retry_passes,
                         uint32_t physical_sector_size, uint64_t transient_errors,
                         uint64_t confirmed_bad_sectors, DWORD last_read_error,
                         bool chunked, double elapsed_seconds, bool finalized) {
    const std::filesystem::path temp(report_path.wstring() + L".tmp");
    {
        std::ofstream report(temp, std::ios::trunc);
        if (!report.is_open()) return false;
        report << "{\n"
               << "  \"format\": \"disk-recovery-imager-report-v1\",\n"
               << "  \"generation\": " << JsonString(map.GenerationId()) << ",\n"
               << "  \"source\": " << JsonString(source) << ",\n"
               << "  \"destination\": " << JsonString(destination) << ",\n"
               << "  \"total_bytes\": " << map.TotalSize() << ",\n"
               << "  \"rescued_bytes\": " << map.TotalRescued() << ",\n"
               << "  \"bad_bytes\": " << map.TotalBad() << ",\n"
               << "  \"pending_bytes\": "
               << (map.TotalSize() - map.TotalRescued() - map.TotalBad()) << ",\n"
               << "  \"sector_size\": " << sector_size << ",\n"
               << "  \"physical_sector_size\": " << physical_sector_size << ",\n"
               << "  \"transient_read_errors\": " << transient_errors << ",\n"
               << "  \"confirmed_bad_sectors\": " << confirmed_bad_sectors << ",\n"
               << "  \"last_read_error\": " << last_read_error << ",\n"
               << "  \"initial_direction\": \"" << (reverse ? "reverse" : "forward") << "\",\n"
               << "  \"retry_passes_requested\": " << retry_passes << ",\n"
               << "  \"chunked\": " << (chunked ? "true" : "false") << ",\n"
               << "  \"elapsed_seconds\": " << std::fixed << std::setprecision(3)
               << elapsed_seconds << ",\n"
               << "  \"finalized\": " << (finalized ? "true" : "false") << "\n"
               << "}\n";
        report.flush();
        if (!report.good()) return false;
    }
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    const bool closed = CloseHandle(handle) != FALSE;
    return flushed && closed && MoveFileExW(temp.c_str(), report_path.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// Aligned buffer required by FILE_FLAG_NO_BUFFERING.
struct AlignedBuffer {
    uint8_t* data = nullptr;
    size_t size = 0;

    explicit AlignedBuffer(size_t sz, size_t alignment = 4096) : size(sz) {
        data = static_cast<uint8_t*>(_aligned_malloc(sz, alignment));
    }
    ~AlignedBuffer() { _aligned_free(data); }
    bool IsValid() const { return data != nullptr; }
};

class DestinationWriter {
public:
    ~DestinationWriter() {
        if (flat_ != INVALID_HANDLE_VALUE) CloseHandle(flat_);
    }

    bool Open(const std::filesystem::path& path, uint64_t total_size, bool resume,
              uint64_t chunk_size,
              const std::vector<recovery::ChunkDestinationSpec>& destinations,
              const std::filesystem::path& source_path, bool lazy_parts) {
        if (!destinations.empty() || (resume && path.extension() == L".rci")) {
            chunked_ = std::make_unique<recovery::ChunkedImageWriter>();
            const bool ok = resume
                ? chunked_->OpenExisting(path, total_size, destinations)
                : chunked_->Create(path, total_size, chunk_size, destinations, source_path,
                                   lazy_parts);
            if (!ok) last_error_ = chunked_->LastErrorCode();
            return ok;
        }
        flat_ = OpenDestImage(path.wstring(), total_size, resume);
        return flat_ != INVALID_HANDLE_VALUE;
    }

    bool Write(uint64_t offset, const void* data, uint32_t size) {
        const bool ok = chunked_ ? chunked_->WriteAt(offset, data, size)
                                 : WriteAt(flat_, offset, data, size);
        if (!ok) last_error_ = chunked_ ? chunked_->LastErrorCode() : GetLastError();
        return ok;
    }

    bool Flush() {
        const bool ok = chunked_ ? chunked_->Flush() : FlushFileBuffers(flat_) != FALSE;
        if (!ok) last_error_ = chunked_ ? chunked_->LastErrorCode() : GetLastError();
        return ok;
    }

    bool Finalize() {
        if (chunked_) {
            const bool ok = chunked_->Finalize();
            if (!ok) last_error_ = chunked_->LastErrorCode();
            return ok;
        }
        if (!Flush()) return false;
        if (!CloseHandle(flat_)) {
            last_error_ = GetLastError();
            return false;
        }
        flat_ = INVALID_HANDLE_VALUE;
        return true;
    }

    DWORD LastErrorCode() const { return last_error_; }

private:
    HANDLE flat_ = INVALID_HANDLE_VALUE;
    std::unique_ptr<recovery::ChunkedImageWriter> chunked_;
    DWORD last_error_ = ERROR_SUCCESS;
};

std::string FormatDuration(double seconds) {
    if (!(seconds >= 0) || seconds > 100.0 * 24 * 3600) return "--:--:--";
    const unsigned long long total = static_cast<unsigned long long>(seconds);
    char text[32];
    if (total >= 24 * 3600) {
        snprintf(text, sizeof(text), "%llud %02llu:%02llu:%02llu", total / 86400,
                 (total / 3600) % 24, (total / 60) % 60, total % 60);
    } else {
        snprintf(text, sizeof(text), "%02llu:%02llu:%02llu", total / 3600, (total / 60) % 60,
                 total % 60);
    }
    return text;
}

// One human-readable status line, rewritten in place. The machine-readable
// state lives in the .map and .report.json files; this is only for the person
// watching the terminal.
void PrintProgressLine(const recovery::RescueMap& map, uint64_t total_size,
                       uint64_t current_offset, double elapsed_seconds,
                       uint64_t resumed_bytes, uint64_t transient_errors,
                       uint64_t confirmed_bad_sectors, const char* phase) {
    const double gib = 1024.0 * 1024.0 * 1024.0;
    const uint64_t rescued = map.TotalRescued();
    const uint64_t bad = map.TotalBad();
    const uint64_t settled = rescued + bad;
    const double percent = total_size ? 100.0 * static_cast<double>(settled) / total_size : 0.0;
    // Rate and ETA are based on what this run actually did, not on bytes that
    // were already in the map when we resumed.
    const uint64_t this_run = rescued > resumed_bytes ? rescued - resumed_bytes : 0;
    const double rate = elapsed_seconds > 0 ? this_run / elapsed_seconds : 0.0;
    const uint64_t remaining = total_size > settled ? total_size - settled : 0;
    const std::string eta = rate > 0 ? FormatDuration(remaining / rate) : "--:--:--";
    printf("\r[%s] %5.1f%%  %.2f/%.2f GB  %.1f MB/s  ETA %s  bad %llu sect (%.2f MB)  "
           "errs %llu  at %.2f GB     ",
           phase, percent, rescued / gib, total_size / gib, rate / (1024.0 * 1024.0),
           eta.c_str(), static_cast<unsigned long long>(confirmed_bad_sectors), bad / (1024.0 * 1024.0),
           static_cast<unsigned long long>(transient_errors), current_offset / gib);
    fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr,
                 L"usage: imager.exe <source> <dest.img|dest.rci> [--resume] [--sector N]\n"
                 L"       [--reverse] [--retry-bad N] [--report PATH]\n"
                 L"       [--read-size SIZE] [--min-read-size SIZE] [--retries N]\n"
                 L"       [--read-timeout-ms N] [--pause-every SIZE --pause-seconds N]\n"
                 L"       [--skip-size SIZE] [--min-dest-free SIZE] [--max-consecutive-failures N]\n"
                 L"       [--chunk-size SIZE --chunk-dest DIR CAPACITY]... [--lazy-parts]\n"
                 L"       [--source-size SIZE]  (image only the first SIZE bytes)\n");
        return 1;
    }

    std::wstring source_path = argv[1];
    std::wstring dest_path = argv[2];
    bool resume = false;
    uint32_t sector_size = 512;
    bool sector_size_overridden = false;
    bool reverse = false;
    int retry_bad_passes = 0;
    uint64_t chunk_size = 0;
    std::vector<recovery::ChunkDestinationSpec> chunk_destinations;
    std::wstring report_path;
    uint64_t read_size = kDefaultChunk;
    uint64_t min_read_size = kMinRetryChunk;
    int retries_per_read = kMaxRetriesPerChunk;
    uint32_t read_timeout_ms = 30000;
    uint64_t pause_every = 0;
    uint32_t pause_seconds = 0;
    uint64_t skip_size = 128ull * 1024 * 1024;
    uint64_t min_dest_free = 0;
    uint64_t source_size_override = 0;
    bool lazy_parts = false;
    uint64_t max_consecutive_failures = 0;

    for (int i = 3; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--resume") {
            resume = true;
        } else if (arg == L"--reverse") {
            reverse = true;
        } else if (arg == L"--retry-bad" && i + 1 < argc) {
            retry_bad_passes = _wtoi(argv[++i]);
            if (retry_bad_passes < 0 || retry_bad_passes > 100) {
                fwprintf(stderr, L"--retry-bad must be between 0 and 100\n");
                return 1;
            }
        } else if (arg == L"--sector" && i + 1 < argc) {
            sector_size = static_cast<uint32_t>(_wtoi(argv[++i]));
            sector_size_overridden = true;
        } else if (arg == L"--chunk-size" && i + 1 < argc) {
            if (!ParseSize(argv[++i], chunk_size)) {
                fwprintf(stderr, L"invalid --chunk-size\n");
                return 1;
            }
        } else if (arg == L"--chunk-dest" && i + 2 < argc) {
            recovery::ChunkDestinationSpec destination;
            destination.root = argv[++i];
            if (!ParseSize(argv[++i], destination.capacity)) {
                fwprintf(stderr, L"invalid --chunk-dest capacity\n");
                return 1;
            }
            chunk_destinations.push_back(std::move(destination));
        } else if (arg == L"--report" && i + 1 < argc) {
            report_path = argv[++i];
        } else if (arg == L"--read-size" && i + 1 < argc) {
            if (!ParseSize(argv[++i], read_size) || read_size > UINT32_MAX) {
                fwprintf(stderr, L"invalid --read-size\n"); return 1;
            }
        } else if (arg == L"--min-read-size" && i + 1 < argc) {
            if (!ParseSize(argv[++i], min_read_size) || min_read_size > UINT32_MAX) {
                fwprintf(stderr, L"invalid --min-read-size\n"); return 1;
            }
        } else if (arg == L"--retries" && i + 1 < argc) {
            retries_per_read = _wtoi(argv[++i]);
            if (retries_per_read < 0 || retries_per_read > 100) {
                fwprintf(stderr, L"--retries must be between 0 and 100\n"); return 1;
            }
        } else if (arg == L"--read-timeout-ms" && i + 1 < argc) {
            const int parsed = _wtoi(argv[++i]);
            if (parsed < 1) { fwprintf(stderr, L"invalid --read-timeout-ms\n"); return 1; }
            read_timeout_ms = static_cast<uint32_t>(parsed);
        } else if (arg == L"--pause-every" && i + 1 < argc) {
            if (!ParseSize(argv[++i], pause_every)) {
                fwprintf(stderr, L"invalid --pause-every\n"); return 1;
            }
        } else if (arg == L"--pause-seconds" && i + 1 < argc) {
            const int parsed = _wtoi(argv[++i]);
            if (parsed < 0 || parsed > 86400) { fwprintf(stderr, L"invalid --pause-seconds\n"); return 1; }
            pause_seconds = static_cast<uint32_t>(parsed);
        } else if (arg == L"--skip-size" && i + 1 < argc) {
            if (!ParseSize(argv[++i], skip_size)) {
                fwprintf(stderr, L"invalid --skip-size\n"); return 1;
            }
        } else if (arg == L"--lazy-parts") {
            lazy_parts = true;
        } else if (arg == L"--max-consecutive-failures" && i + 1 < argc) {
            const int parsed = _wtoi(argv[++i]);
            if (parsed < 0) { fwprintf(stderr, L"invalid --max-consecutive-failures\n"); return 1; }
            max_consecutive_failures = static_cast<uint64_t>(parsed);
        } else if (arg == L"--source-size" && i + 1 < argc) {
            if (!ParseSize(argv[++i], source_size_override) || source_size_override == 0) {
                fwprintf(stderr, L"invalid --source-size\n"); return 1;
            }
        } else if (arg == L"--min-dest-free" && i + 1 < argc) {
            if (!ParseSize(argv[++i], min_dest_free) || min_dest_free == 0) {
                fwprintf(stderr, L"invalid --min-dest-free\n"); return 1;
            }
        } else {
            fwprintf(stderr, L"unknown or incomplete option: %ls\n", arg.c_str());
            return 1;
        }
    }

    const bool chunked_resume = resume && chunk_destinations.empty() && chunk_size == 0 &&
        std::filesystem::path(dest_path).extension() == L".rci";
    if (!chunked_resume && chunk_destinations.empty() != (chunk_size == 0)) {
        fwprintf(stderr, L"--chunk-size and at least one --chunk-dest must be used together\n");
        return 1;
    }
    if (lazy_parts && chunk_destinations.empty() && !chunked_resume) {
        fwprintf(stderr, L"--lazy-parts requires --chunk-size/--chunk-dest\n");
        return 1;
    }
    if (!chunk_destinations.empty() && chunk_size % sector_size != 0 && sector_size_overridden) {
        fwprintf(stderr, L"--chunk-size must be a multiple of the sector size\n");
        return 1;
    }

    if (NormalizedPath(source_path) == NormalizedPath(dest_path)) {
        fwprintf(stderr, L"source and destination must be different\n");
        return 1;
    }

    HANDLE src = OpenSourceForRawRead(source_path);
    if (!sector_size_overridden) sector_size = GetLogicalSectorSize(src);
    if (sector_size < 512 || sector_size > 65536 || (sector_size & (sector_size - 1)) != 0) {
        fwprintf(stderr, L"invalid sector size %u\n", sector_size);
        CloseHandle(src);
        return 1;
    }
    const uint32_t physical_sector_size = GetPhysicalSectorSize(src, sector_size);
    const uint32_t io_alignment = std::max<uint32_t>(4096, physical_sector_size);
    if (read_size < sector_size || read_size % sector_size != 0 ||
        min_read_size < sector_size || min_read_size > read_size ||
        min_read_size % sector_size != 0) {
        fwprintf(stderr, L"read sizes must be sector-aligned and min-read-size <= read-size\n");
        CloseHandle(src);
        return 1;
    }
    if (chunk_size != 0 && chunk_size % sector_size != 0) {
        fwprintf(stderr, L"--chunk-size must be a multiple of the sector size\n");
        CloseHandle(src);
        return 1;
    }
    uint64_t total_size = GetSourceSize(src);
    if (source_size_override != 0) {
        // Fake-capacity drives: image only the part that really exists.
        if (source_size_override > total_size) {
            fwprintf(stderr, L"--source-size is larger than the reported device size\n");
            CloseHandle(src);
            return 1;
        }
        printf("reported size: %.2f GB, limiting to --source-size\n",
               total_size / (1024.0 * 1024.0 * 1024.0));
        total_size = source_size_override;
    }
    if (total_size == 0) {
        fwprintf(stderr, L"could not determine source size\n");
        CloseHandle(src);
        return 1;
    }
    if (total_size % sector_size != 0) {
        fwprintf(stderr, L"source size is not a multiple of the logical sector size\n");
        CloseHandle(src);
        return 1;
    }
    printf("source size: %.2f GB\n", total_size / (1024.0 * 1024.0 * 1024.0));

    std::string map_path_narrow = WideToUtf8(dest_path) + ".map";
    if (report_path.empty()) report_path = dest_path + L".report.json";
    const std::string source_identity = WideToUtf8(NormalizedPath(source_path));
    const std::string destination_identity = WideToUtf8(NormalizedPath(dest_path));

    recovery::RescueMap map(total_size);
    map.ConfigureIdentity(source_identity, destination_identity, sector_size);
    uint64_t resumed_bytes = 0;
    if (resume) {
        if (map.Load(map_path_narrow)) {
            resumed_bytes = map.TotalRescued();
            printf("resumed from %s (%.2f GB already rescued, %.2f MB bad, %.2f GB still pending)\n",
                   map_path_narrow.c_str(), resumed_bytes / (1024.0 * 1024.0 * 1024.0),
                   map.TotalBad() / (1024.0 * 1024.0),
                   (total_size - resumed_bytes - map.TotalBad()) / (1024.0 * 1024.0 * 1024.0));
        } else {
            fprintf(stderr, "resume requested but no valid map exists at %s (or .bak)\n",
                    map_path_narrow.c_str());
            CloseHandle(src);
            return 1;
        }
    }

    DestinationWriter destination;
    const bool chunked_output = !chunk_destinations.empty() || chunked_resume;
    if (lazy_parts && !chunk_destinations.empty()) {
        printf("lazy parts: part files are created only where non-zero data is read; "
               "all-zero regions cost no destination space\n");
    }
    if (!destination.Open(dest_path, total_size, resume, chunk_size, chunk_destinations,
                          source_path, lazy_parts)) {
        fprintf(stderr, "could not open destination, error %lu\n", destination.LastErrorCode());
        CloseHandle(src);
        return 1;
    }

    AlignedBuffer buffer(static_cast<size_t>(read_size), io_alignment);
    if (!buffer.IsValid()) {
        fprintf(stderr, "could not allocate aligned read buffer\n");
        CloseHandle(src);
        return 1;
    }
    auto last_checkpoint = std::chrono::steady_clock::now();
    auto last_progress_print = last_checkpoint;
    uint64_t rescued_since_checkpoint = 0;
    auto start_time = std::chrono::steady_clock::now();
    uint64_t transient_errors = 0;
    uint64_t confirmed_bad_sectors = 0;
    DWORD last_read_error = ERROR_SUCCESS;
    uint64_t bytes_since_pause = 0;
    uint64_t consecutive_failures = 0;

    for (int imaging_pass = 0; imaging_pass < (skip_size == 0 ? 1 : 2); ++imaging_pass) {
      const bool fast_pass = skip_size != 0 && imaging_pass == 0;
      const bool pass_reverse = imaging_pass == 0 ? reverse : !reverse;
      if (!fast_pass) {
          const auto deferred = map.RangesWithStatus(recovery::RangeStatus::Deferred);
          for (const auto& range : deferred) map.MarkNotTried(range.offset, range.size);
          if (!deferred.empty())
              printf("\ntrimming/scraping %zu deferred ranges (%s)\n", deferred.size(),
                     pass_reverse ? "reverse" : "forward");
      }
      for (;;) {
        const uint64_t pending_limit = fast_pass ? std::max(read_size, skip_size) : read_size;
        recovery::Range pending = map.NextPending(pending_limit, pass_reverse);
        if (pending.size == 0) break;

        uint64_t offset = pending.offset;
        uint64_t remaining = pending.size;
        uint64_t current_read_size = read_size;

        while (remaining > 0) {
            uint64_t this_chunk = std::min(current_read_size, remaining);
            // Keep reads sector-aligned in size.
            this_chunk -= (this_chunk % sector_size);
            if (this_chunk == 0) this_chunk = sector_size;

            bool ok = false;
            uint32_t got = 0;
            for (int attempt = 0; attempt <= retries_per_read && !ok; ++attempt) {
                DWORD error = ERROR_SUCCESS;
                ok = ReadAt(src, offset, buffer.data, static_cast<uint32_t>(this_chunk),
                            read_timeout_ms, &got, &error);
                if (!ok) {
                    ++transient_errors;
                    last_read_error = error;
                }
            }
            consecutive_failures = ok ? 0 : consecutive_failures + 1;
            if (max_consecutive_failures != 0 && consecutive_failures >= max_consecutive_failures) {
                // A drive that answers nothing at all (USB bridge reset, power
                // loss, "not ready") must not be raced across at skip-size
                // steps, marking everything deferred. Checkpoint and stop so the
                // user can power-cycle it and --resume.
                if (!destination.Flush() || !map.Save(map_path_narrow)) {
                    fprintf(stderr, "\ncheckpoint failed while stopping, error %lu\n",
                            destination.LastErrorCode());
                    CloseHandle(src);
                    return 1;
                }
                const double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start_time).count();
                WriteProgressReport(report_path, map, source_identity, destination_identity,
                                    sector_size, reverse, retry_bad_passes,
                                    physical_sector_size, transient_errors,
                                    confirmed_bad_sectors, last_read_error,
                                    chunked_output, elapsed, false);
                printf("\n%llu consecutive reads failed (last Windows error %lu); the drive is not "
                       "responding. Stopped cleanly after %.2f GB rescued. Power-cycle the drive "
                       "and run again with --resume.\n",
                       static_cast<unsigned long long>(consecutive_failures),
                       static_cast<unsigned long>(last_read_error),
                       map.TotalRescued() / (1024.0 * 1024.0 * 1024.0));
                CloseHandle(src);
                return kExitDriveUnresponsive;
            }

            if (ok) {
                if (got > 0) {
                    if (!destination.Write(offset, buffer.data, got)) {
                        fprintf(stderr, "destination write failed, error %lu\n", destination.LastErrorCode());
                        CloseHandle(src);
                        return 1;
                    }
                    map.MarkRescued(offset, got);
                    rescued_since_checkpoint += got;
                    bytes_since_pause += got;
                }
                offset += this_chunk;
                remaining -= this_chunk;
                // Ramp chunk size back up after a successful read.
                current_read_size = std::min(current_read_size * 2, read_size);
                if (pause_every != 0 && bytes_since_pause >= pause_every && pause_seconds != 0) {
                    printf("\npausing %u seconds after %llu bytes for drive cooling\n",
                           pause_seconds, static_cast<unsigned long long>(bytes_since_pause));
                    Sleep(pause_seconds * 1000);
                    bytes_since_pause = 0;
                }
            } else if (fast_pass && this_chunk > sector_size) {
                uint64_t deferred_size = std::min<uint64_t>(remaining,
                    std::max<uint64_t>(skip_size, this_chunk));
                deferred_size -= deferred_size % sector_size;
                if (deferred_size == 0) deferred_size = sector_size;
                map.MarkDeferred(offset, deferred_size);
                offset += deferred_size;
                remaining -= deferred_size;
                current_read_size = read_size;
            } else if (this_chunk > sector_size) {
                // Back off and retry this same offset with a smaller chunk.
                current_read_size = this_chunk > min_read_size
                    ? std::max(this_chunk / 4, min_read_size)
                    : static_cast<uint64_t>(sector_size);
            } else {
                // A single sector is unreadable; record it as bad and move on.
                map.MarkBad(offset, sector_size);
                ++confirmed_bad_sectors;
                offset += sector_size;
                remaining -= std::min(remaining, static_cast<uint64_t>(sector_size));
                current_read_size = read_size;
            }

            auto now = std::chrono::steady_clock::now();
            if (rescued_since_checkpoint >= kCheckpointEveryBytes ||
                now - last_checkpoint > std::chrono::seconds(30)) {
                // The image must reach stable storage before the map claims
                // that the corresponding bytes are safely rescued.
                if (!destination.Flush()) {
                    fprintf(stderr, "destination flush failed, error %lu\n", destination.LastErrorCode());
                    CloseHandle(src);
                    return 1;
                }
                if (!map.Save(map_path_narrow)) {
                    fprintf(stderr, "\nfailed to checkpoint rescue map %s\n", map_path_narrow.c_str());
                    CloseHandle(src);
                    return 1;
                }
                const double elapsed = std::chrono::duration<double>(now - start_time).count();
                if (!WriteProgressReport(report_path, map, source_identity, destination_identity,
                                         sector_size, reverse, retry_bad_passes,
                                         physical_sector_size, transient_errors,
                                         confirmed_bad_sectors, last_read_error,
                                         chunked_output, elapsed, false)) {
                    fprintf(stderr, "\nfailed to checkpoint progress report\n");
                    CloseHandle(src);
                    return 1;
                }
                last_checkpoint = now;
                rescued_since_checkpoint = 0;

                if (min_dest_free != 0) {
                    ULARGE_INTEGER free_bytes{};
                    const std::wstring dest_dir = DestinationDirectory(dest_path);
                    if (!GetDiskFreeSpaceExW(dest_dir.c_str(), &free_bytes, nullptr, nullptr)) {
                        fprintf(stderr, "\ncannot query destination free space, error %lu\n",
                                GetLastError());
                        CloseHandle(src);
                        return 1;
                    }
                    if (free_bytes.QuadPart < min_dest_free) {
                        // Everything rescued so far is flushed and checkpointed above.
                        printf("\ndestination free space %.2f GB is below --min-dest-free %.2f GB; "
                               "stopping cleanly after %.2f GB rescued. Continue later with --resume "
                               "once space is available.\n",
                               free_bytes.QuadPart / (1024.0 * 1024.0 * 1024.0),
                               min_dest_free / (1024.0 * 1024.0 * 1024.0),
                               map.TotalRescued() / (1024.0 * 1024.0 * 1024.0));
                        CloseHandle(src);
                        return kExitDestinationLow;
                    }
                }

            }
            if (now - last_progress_print > std::chrono::seconds(2)) {
                const double elapsed = std::chrono::duration<double>(now - start_time).count();
                PrintProgressLine(map, total_size, offset, elapsed, resumed_bytes,
                                  transient_errors, confirmed_bad_sectors,
                                  fast_pass ? "fast" : "scrape");
                last_progress_print = now;
            }
        }
      }
    }

    for (int pass = 0; pass < retry_bad_passes; ++pass) {
        const bool retry_reverse = ((pass & 1) != 0) ^ reverse;
        const auto bad_ranges = map.RangesWithStatus(recovery::RangeStatus::Bad, retry_reverse);
        if (bad_ranges.empty()) break;
        printf("\nretry pass %d/%d (%s, %zu bad ranges)\n", pass + 1, retry_bad_passes,
               retry_reverse ? "reverse" : "forward", bad_ranges.size());
        for (const auto& range : bad_ranges) {
            uint64_t progressed = 0;
            while (progressed < range.size) {
                const uint64_t relative = retry_reverse
                    ? range.size - progressed - std::min<uint64_t>(sector_size, range.size - progressed)
                    : progressed;
                const uint64_t offset = range.offset + relative;
                const uint32_t amount = static_cast<uint32_t>(
                    std::min<uint64_t>(sector_size, range.size - relative));
                uint32_t got = 0;
                DWORD error = ERROR_SUCCESS;
                if (ReadAt(src, offset, buffer.data, amount, read_timeout_ms, &got, &error) &&
                    got == amount) {
                    if (!destination.Write(offset, buffer.data, got)) {
                        fprintf(stderr, "destination retry write failed, error %lu\n",
                                destination.LastErrorCode());
                        CloseHandle(src);
                        return 1;
                    }
                    map.MarkRescued(offset, got);
                } else {
                    ++transient_errors;
                    last_read_error = error;
                }
                progressed += amount;
                const auto now = std::chrono::steady_clock::now();
                if (now - last_progress_print > std::chrono::seconds(2)) {
                    PrintProgressLine(map, total_size, offset,
                                      std::chrono::duration<double>(now - start_time).count(),
                                      resumed_bytes, transient_errors, confirmed_bad_sectors,
                                      "retry");
                    last_progress_print = now;
                }
            }
        }
        printf("\nretry pass %d done: %.2f MB still bad\n", pass + 1,
               map.TotalBad() / (1024.0 * 1024.0));
        if (!destination.Flush()) {
            fprintf(stderr, "destination retry flush failed, error %lu\n",
                    destination.LastErrorCode());
            CloseHandle(src);
            return 1;
        }
        if (!map.Save(map_path_narrow)) {
            fprintf(stderr, "failed to checkpoint retry pass\n");
            CloseHandle(src);
            return 1;
        }
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        if (!WriteProgressReport(report_path, map, source_identity, destination_identity,
                                 sector_size, reverse, retry_bad_passes,
                                 physical_sector_size, transient_errors,
                                 confirmed_bad_sectors, last_read_error,
                                 chunked_output, elapsed, false)) {
            fprintf(stderr, "failed to checkpoint retry report\n");
            CloseHandle(src);
            return 1;
        }
    }

    if (!destination.Flush()) {
        fprintf(stderr, "destination final flush failed, error %lu\n", destination.LastErrorCode());
        CloseHandle(src);
        return 1;
    }
    if (!map.Save(map_path_narrow)) {
        fprintf(stderr, "\nfailed to save rescue map %s\n", map_path_narrow.c_str());
        CloseHandle(src);
        return 1;
    }
    if (!destination.Finalize()) {
        fprintf(stderr, "destination finalization failed, error %lu\n",
                destination.LastErrorCode());
        CloseHandle(src);
        return 1;
    }
    const double final_elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_time).count();
    if (!WriteProgressReport(report_path, map, source_identity, destination_identity,
                             sector_size, reverse, retry_bad_passes,
                             physical_sector_size, transient_errors,
                             confirmed_bad_sectors, last_read_error,
                             chunked_output, final_elapsed, true)) {
        fprintf(stderr, "failed to write final progress report\n");
        CloseHandle(src);
        return 1;
    }
    printf("\ndone in %s. rescued %.2f GB, %.2f MB unreadable in %llu sectors, "
           "%llu transient read errors (see %s)\n",
           FormatDuration(final_elapsed).c_str(),
           map.TotalRescued() / (1024.0 * 1024.0 * 1024.0),
           map.TotalBad() / (1024.0 * 1024.0),
           static_cast<unsigned long long>(confirmed_bad_sectors),
           static_cast<unsigned long long>(transient_errors), map_path_narrow.c_str());

    if (!CloseHandle(src)) {
        fprintf(stderr, "source close failed, error %lu\n", GetLastError());
        return 1;
    }
    return 0;
}

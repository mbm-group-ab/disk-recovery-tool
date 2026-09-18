// carver: signature-based file carving fallback, for data that exfat-parser
// couldn't recover because its directory entry is gone or unreadable.
//
// This does NOT recover original names or folder structure -- it only finds
// byte patterns that look like known file headers and cuts out a file
// starting there (up to a matching footer if the format has one, otherwise
// up to a size cap). Use it after exfat-parser, on whatever region of the
// image wasn't already claimed, to mop up what metadata recovery missed.
//
// Usage:
//   carver.exe disk.img D:\carved

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "rescue_map.h"
#include "random_access_reader.h"
#include "sha256.h"
#include "win32_paths.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlockSize = 16 * 1024 * 1024;   // scan window
constexpr size_t kOverlap = 4096;                  // handles signatures spanning block boundary

struct Signature {
    const char* ext;
    std::vector<uint8_t> header;
    std::vector<uint8_t> footer;  // empty = no footer, use max_size cutoff
    uint64_t max_size;
    enum class Validator { Footer, Zip, Mp4 } validator = Validator::Footer;
    size_t header_backtrack = 0;
};

const std::vector<Signature>& Signatures() {
    static const std::vector<Signature> sigs = {
        {"jpg", {0xFF, 0xD8, 0xFF}, {0xFF, 0xD9}, 64ull * 1024 * 1024},
        {"png", {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}, 128ull * 1024 * 1024},
        {"pdf", {'%', 'P', 'D', 'F', '-'}, {'%', '%', 'E', 'O', 'F'}, 256ull * 1024 * 1024},
        {"gif", {'G', 'I', 'F', '8'}, {0x3B}, 64ull * 1024 * 1024},
        {"zip", {'P', 'K', 0x03, 0x04}, {}, 1024ull * 1024 * 1024,
         Signature::Validator::Zip},
        // The searchable marker is at byte four of the first ISO BMFF box.
        {"mp4", {'f', 't', 'y', 'p'}, {}, 1024ull * 1024 * 1024,
         Signature::Validator::Mp4, 4},
    };
    return sigs;
}

int64_t FindSequence(const uint8_t* data, size_t len,
                     const std::vector<uint8_t>& needle, size_t from);
std::optional<uint64_t> FindFooterLength(recovery::RandomAccessReader& source,
                                         uint64_t file_start, uint64_t total_size,
                                         const Signature& sig);

uint32_t ReadLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t ReadLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadBe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint64_t ReadBe64(const uint8_t* data) {
    return (static_cast<uint64_t>(ReadBe32(data)) << 32) | ReadBe32(data + 4);
}

std::optional<uint64_t> FindZipLength(recovery::RandomAccessReader& source,
                                      uint64_t file_start, uint64_t total_size,
                                      uint64_t max_size) {
    static const std::vector<uint8_t> eocd = {'P', 'K', 0x05, 0x06};
    constexpr size_t block_size = 4 * 1024 * 1024;
    std::vector<uint8_t> data(block_size + 3);
    const uint64_t limit = std::min<uint64_t>(max_size, total_size - file_start);
    uint64_t relative = 4;
    while (relative < limit) {
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(data.size(), limit - relative));
        if (!source.ReadAt(file_start + relative, data.data(), amount)) return std::nullopt;
        size_t search_from = 0;
        while (search_from < amount) {
            const int64_t found = FindSequence(data.data(), amount, eocd, search_from);
            if (found < 0) break;
            const uint64_t eocd_relative = relative + static_cast<uint64_t>(found);
            uint8_t record[22]{};
            if (eocd_relative + sizeof(record) <= limit &&
                source.ReadAt(file_start + eocd_relative, record, sizeof(record))) {
                const uint16_t disk = ReadLe16(record + 4);
                const uint16_t directory_disk = ReadLe16(record + 6);
                const uint16_t entries_here = ReadLe16(record + 8);
                const uint16_t entries_total = ReadLe16(record + 10);
                const uint32_t directory_size = ReadLe32(record + 12);
                const uint32_t directory_offset = ReadLe32(record + 16);
                const uint16_t comment_length = ReadLe16(record + 20);
                const uint64_t end = eocd_relative + sizeof(record) + comment_length;
                const bool directory_fits = directory_offset <= eocd_relative &&
                    directory_size <= eocd_relative - directory_offset;
                bool central_signature_valid = entries_total == 0;
                if (entries_total != 0 && directory_offset + 4 <= limit) {
                    uint8_t central[4]{};
                    central_signature_valid = source.ReadAt(file_start + directory_offset,
                                                            central, sizeof(central)) &&
                        memcmp(central, "PK\x01\x02", 4) == 0;
                }
                if (disk == 0 && directory_disk == 0 && entries_here == entries_total &&
                    directory_fits && central_signature_valid && end <= limit) return end;
            }
            search_from = static_cast<size_t>(found) + 1;
        }
        if (amount <= 3) break;
        relative += amount - 3;
    }
    return std::nullopt;
}

std::optional<uint64_t> FindMp4Length(recovery::RandomAccessReader& source,
                                      uint64_t file_start, uint64_t total_size,
                                      uint64_t max_size) {
    const uint64_t limit = std::min<uint64_t>(max_size, total_size - file_start);
    uint64_t cursor = 0;
    bool saw_ftyp = false, saw_moov = false, saw_mdat = false;
    while (cursor + 8 <= limit) {
        uint8_t header[16]{};
        if (!source.ReadAt(file_start + cursor, header, 8)) return std::nullopt;
        uint64_t box_size = ReadBe32(header);
        uint64_t header_size = 8;
        if (box_size == 1) {
            if (cursor + 16 > limit || !source.ReadAt(file_start + cursor + 8, header + 8, 8))
                return std::nullopt;
            box_size = ReadBe64(header + 8);
            header_size = 16;
        }
        if (box_size < header_size || box_size > limit - cursor) break;
        const std::string type(reinterpret_cast<char*>(header + 4), 4);
        if (cursor == 0 && type != "ftyp") return std::nullopt;
        saw_ftyp = saw_ftyp || type == "ftyp";
        saw_moov = saw_moov || type == "moov";
        saw_mdat = saw_mdat || type == "mdat";
        cursor += box_size;
        if (cursor == limit) break;
    }
    return saw_ftyp && saw_moov && saw_mdat && cursor > 0 ? std::optional<uint64_t>(cursor)
                                                          : std::nullopt;
}

std::optional<uint64_t> FindValidatedLength(recovery::RandomAccessReader& source,
                                            uint64_t file_start, uint64_t total_size,
                                            const Signature& sig) {
    if (sig.validator == Signature::Validator::Zip)
        return FindZipLength(source, file_start, total_size, sig.max_size);
    if (sig.validator == Signature::Validator::Mp4)
        return FindMp4Length(source, file_start, total_size, sig.max_size);
    return FindFooterLength(source, file_start, total_size, sig);
}

std::optional<uint64_t> FindFooterLength(recovery::RandomAccessReader& source,
                                         uint64_t file_start, uint64_t total_size,
                                         const Signature& sig) {
    if (sig.footer.empty() || file_start >= total_size) return std::nullopt;
    constexpr size_t kSearchBlock = 4 * 1024 * 1024;
    const size_t overlap = sig.footer.size() - 1;
    std::vector<uint8_t> data(kSearchBlock + overlap);
    const uint64_t limit = std::min<uint64_t>(sig.max_size, total_size - file_start);
    uint64_t relative = 0;
    while (relative < limit) {
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(data.size(), limit - relative));
        if (!source.ReadAt(file_start + relative, data.data(), amount)) return std::nullopt;
        const size_t got = amount;
        const size_t from = relative == 0 ? sig.header.size() : 0;
        const int64_t found = FindSequence(data.data(), got, sig.footer, from);
        if (found >= 0)
            return relative + static_cast<uint64_t>(found) + sig.footer.size();
        if (got <= overlap) break;
        relative += got - overlap;
    }
    return std::nullopt;
}

int64_t FindSequence(const uint8_t* data, size_t len, const std::vector<uint8_t>& needle, size_t from) {
    if (needle.empty() || from >= len) return -1;
    auto it = std::search(data + from, data + len, needle.begin(), needle.end());
    if (it == data + len) return -1;
    return it - data;
}

fs::path VerificationPath(const fs::path& output) {
    return fs::path(output.string() + ".carve.sha256");
}

std::optional<std::string> VerifiedExistingHash(const fs::path& output, uint64_t expected_size) {
    std::error_code ec;
    if (!fs::is_regular_file(output, ec) || ec || fs::file_size(output, ec) != expected_size || ec)
        return std::nullopt;
    std::ifstream sidecar(VerificationPath(output));
    uint64_t recorded_size = 0;
    std::string recorded_hash, extra;
    if (!(sidecar >> recorded_size >> recorded_hash) || sidecar >> extra ||
        recorded_size != expected_size || recorded_hash.size() != 64) return std::nullopt;
    const auto actual = recovery::Sha256File(output);
    if (!actual || *actual != recorded_hash) return std::nullopt;
    return actual;
}

bool WriteVerification(const fs::path& output, uint64_t size, const std::string& hash) {
    const fs::path sidecar = VerificationPath(output);
    const fs::path temp(sidecar.string() + ".tmp");
    {
        std::ofstream record(temp, std::ios::trunc);
        if (!record.is_open()) return false;
        record << size << ' ' << hash << '\n';
        record.flush();
        if (!record.good()) return false;
    }
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    const bool closed = CloseHandle(handle) != FALSE;
    return flushed && closed && MoveFileExW(temp.c_str(), sidecar.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: recovery_carver.exe <image.img|image.rci> <output_dir>\n"
                        "       (--whole-image | --range OFFSET:LENGTH)...\n"
                        "       [--max-output bytes] [--map path] [--include-partial]\n");
        return 1;
    }
    std::string image_path = argv[1];
    fs::path out_dir = argv[2];
    uint64_t max_output = 0;
    std::string map_path;
    bool include_partial = false;
    bool whole_image = false;
    std::vector<std::string> requested_ranges;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--max-output" && i + 1 < argc) {
            try {
                size_t consumed = 0;
                std::string val = argv[++i];
                max_output = std::stoull(val, &consumed);
                if (consumed != val.size()) throw std::invalid_argument("trailing junk");
            } catch (const std::exception&) {
                fprintf(stderr, "invalid --max-output value '%s'\n", argv[i]);
                return 1;
            }
        } else if (std::string(argv[i]) == "--map" && i + 1 < argc) {
            map_path = argv[++i];
        } else if (std::string(argv[i]) == "--include-partial") {
            include_partial = true;
        } else if (std::string(argv[i]) == "--whole-image") {
            whole_image = true;
        } else if (std::string(argv[i]) == "--range" && i + 1 < argc) {
            requested_ranges.emplace_back(argv[++i]);
        } else {
            fprintf(stderr, "unrecognized argument: %s\n", argv[i]);
            return 1;
        }
    }
    std::error_code dir_error;
    recovery::CreateDirectoriesWin32(out_dir, dir_error);
    if (dir_error || !fs::is_directory(out_dir)) {
        fprintf(stderr, "cannot create output directory %s\n", out_dir.string().c_str());
        return 1;
    }

    recovery::RandomAccessReader img;
    if (!img.Open(image_path)) {
        fprintf(stderr, "cannot open %s\n", image_path.c_str());
        return 1;
    }
    const uint64_t total_size = img.Size();

    if (whole_image && !requested_ranges.empty()) {
        fprintf(stderr, "--whole-image and --range cannot be combined\n");
        return 1;
    }
    std::vector<recovery::Range> scan_ranges;
    if (whole_image) {
        scan_ranges.push_back({0, total_size, recovery::RangeStatus::NotTried});
    } else {
        for (const auto& specification : requested_ranges) {
            const size_t separator = specification.find(':');
            try {
                size_t offset_end = 0, size_end = 0;
                const uint64_t offset = std::stoull(specification.substr(0, separator), &offset_end);
                const uint64_t size = separator == std::string::npos ? 0 :
                    std::stoull(specification.substr(separator + 1), &size_end);
                if (separator == std::string::npos || offset_end != separator ||
                    size_end != specification.size() - separator - 1 || size == 0 ||
                    offset > total_size || size > total_size - offset) throw std::invalid_argument("range");
                scan_ranges.push_back({offset, size, recovery::RangeStatus::NotTried});
            } catch (...) {
                fprintf(stderr, "invalid --range: %s\n", specification.c_str());
                return 1;
            }
        }
        if (scan_ranges.empty()) {
            fprintf(stderr, "refusing an implicit whole-image scan; use --whole-image or --range\n");
            return 1;
        }
        std::sort(scan_ranges.begin(), scan_ranges.end(),
                  [](const auto& a, const auto& b) { return a.offset < b.offset; });
        for (size_t i = 1; i < scan_ranges.size(); ++i) {
            if (scan_ranges[i].offset < scan_ranges[i - 1].offset + scan_ranges[i - 1].size) {
                fprintf(stderr, "--range values must not overlap\n");
                return 1;
            }
        }
    }

    if (map_path.empty() && fs::exists(image_path + ".map")) map_path = image_path + ".map";
    std::unique_ptr<recovery::RescueMap> rescue_map;
    if (!map_path.empty()) {
        rescue_map = std::make_unique<recovery::RescueMap>(total_size);
        if (!rescue_map->Load(map_path)) {
            fprintf(stderr, "invalid rescue map: %s\n", map_path.c_str());
            return 1;
        }
    }

    std::ofstream manifest(out_dir / "carve_manifest.csv", std::ios::trunc);
    if (!manifest.is_open()) {
        fprintf(stderr, "cannot create carve manifest\n");
        return 1;
    }
    manifest << "source_offset,length,extension,status,unrescued_bytes,output,sha256\n";

    std::vector<uint8_t> block(kBlockSize + kOverlap);
    int found_count = 0;
    uint64_t output_bytes = 0;
    std::unordered_map<std::string, fs::path> content_by_hash;

    printf("carving %.2f GB image...\n", total_size / (1024.0 * 1024.0 * 1024.0));

    for (const auto& scan_range : scan_ranges) {
      uint64_t base_offset = scan_range.offset;
      const uint64_t scan_end = scan_range.offset + scan_range.size;
      while (base_offset < scan_end) {
        size_t to_read = static_cast<size_t>(std::min<uint64_t>(block.size(), scan_end - base_offset));
        if (!img.ReadAt(base_offset, block.data(), to_read)) {
            fprintf(stderr, "failed reading source at offset %llu\n",
                    static_cast<unsigned long long>(base_offset));
            return 1;
        }
        const size_t got = to_read;

        const size_t scan_limit = got < block.size() ? got : std::min(got, kBlockSize);
        for (size_t pos = 0; pos < scan_limit; ) {
            bool matched = false;
            for (const auto& sig : Signatures()) {
                if (pos + sig.header.size() > got) continue;
                if (memcmp(block.data() + pos, sig.header.data(), sig.header.size()) != 0) {
                    continue;
                }

                if (base_offset + pos < scan_range.offset + sig.header_backtrack) continue;
                uint64_t file_start_in_block = pos - sig.header_backtrack;
                uint64_t file_start_abs = base_offset + file_start_in_block;

                const auto length = FindValidatedLength(img, file_start_abs, scan_end, sig);
                if (!length) continue;
                const uint64_t file_len = *length;
                const uint64_t unrescued = rescue_map
                    ? rescue_map->UnrescuedBytes(file_start_abs, file_len) : 0;
                if (unrescued != 0 && !include_partial) {
                    manifest << file_start_abs << ',' << file_len << ',' << sig.ext
                             << ",skipped-partial," << unrescued << ",,\n";
                    pos += std::max<size_t>(sig.header.size(), 1);
                    matched = true;
                    break;
                }
                if ((max_output != 0 && file_len > max_output - std::min(max_output, output_bytes))) {
                    fprintf(stderr, "\noutput cap reached after %llu bytes\n",
                            (unsigned long long)output_bytes);
                    return 0;
                }
                std::error_code space_error;
                const auto space = fs::space(out_dir, space_error);
                if (!space_error && space.available < file_len) {
                    fprintf(stderr, "\nnot enough free space for next carved file\n");
                    return 0;
                }

                char name[64];
                snprintf(name, sizeof(name), "carved_0x%llx.%s",
                         (unsigned long long)file_start_abs, sig.ext);
                fs::path out_path = out_dir / name;
                if (fs::exists(out_path)) {
                    const auto verified_hash = VerifiedExistingHash(out_path, file_len);
                    if (!verified_hash) {
                        fprintf(stderr, "\nexisting carved output is not verified: %s\n",
                                out_path.string().c_str());
                        return 1;
                    }
                    content_by_hash.emplace(*verified_hash, out_path);
                    manifest << file_start_abs << ',' << file_len << ',' << sig.ext
                             << ",verified-existing," << unrescued << ",\""
                             << out_path.string() << "\"," << *verified_hash << "\n";
                    ++found_count;
                    pos += std::max<size_t>(sig.header.size(), 1);
                    matched = true;
                    break;
                }

                std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
                std::vector<char> filebuf(static_cast<size_t>(std::min<uint64_t>(file_len, 64ull * 1024 * 1024)));
                uint64_t remaining = file_len;
                uint64_t source_cursor = file_start_abs;
                while (remaining > 0) {
                    size_t chunk = static_cast<size_t>(std::min<uint64_t>(filebuf.size(), remaining));
                    if (!img.ReadAt(source_cursor, filebuf.data(), chunk)) break;
                    out.write(filebuf.data(), static_cast<std::streamsize>(chunk));
                    if (!out.good()) break;
                    remaining -= chunk;
                    source_cursor += chunk;
                }

                out.flush();
                if (remaining != 0 || !out.good()) {
                    fprintf(stderr, "\nfailed writing %s\n", out_path.string().c_str());
                    std::error_code remove_error;
                    fs::remove(out_path, remove_error);
                    return 1;
                }
                out.close();
                if (!out.good()) {
                    fprintf(stderr, "\nfailed closing %s\n", out_path.string().c_str());
                    std::error_code remove_error;
                    fs::remove(out_path, remove_error);
                    return 1;
                }
                const auto hash = recovery::Sha256File(out_path);
                if (!hash) {
                    fprintf(stderr, "\nfailed hashing %s\n", out_path.string().c_str());
                    return 1;
                }
                const auto duplicate = content_by_hash.find(*hash);
                if (duplicate != content_by_hash.end()) {
                    std::error_code remove_error;
                    if (!fs::remove(out_path, remove_error) || remove_error) {
                        fprintf(stderr, "\nfailed removing duplicate output\n");
                        return 1;
                    }
                    manifest << file_start_abs << ',' << file_len << ',' << sig.ext
                             << ",duplicate-content," << unrescued << ",\""
                             << duplicate->second.string() << "\"," << *hash << "\n";
                    ++found_count;
                    pos += std::max<size_t>(sig.header.size(), 1);
                    matched = true;
                    break;
                }
                if (!WriteVerification(out_path, file_len, *hash)) {
                    fprintf(stderr, "\nfailed writing verification sidecar\n");
                    return 1;
                }
                content_by_hash.emplace(*hash, out_path);
                output_bytes += file_len;
                manifest << file_start_abs << ',' << file_len << ',' << sig.ext << ','
                         << (unrescued == 0 ? "complete" : "partial") << ',' << unrescued
                         << ",\"" << out_path.string() << "\"," << *hash << "\n";

                ++found_count;
                if (found_count % 50 == 0) printf("\rcarved %d files...   ", found_count);
                pos += std::max<size_t>(sig.header.size(), 1);
                matched = true;
                break;
            }
            if (!matched) ++pos;
        }

        if (got < block.size()) break;
        base_offset += (kBlockSize);  // overlap region re-scanned for boundary-spanning signatures
      }
    }

    manifest.flush();
    manifest.close();
    if (!manifest.good()) {
        fprintf(stderr, "\nfailed writing carve manifest\n");
        return 1;
    }
    printf("\ndone. carved %d files into %s\n", found_count, out_dir.string().c_str());
    return 0;
}

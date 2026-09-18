#include "chunked_image_writer.h"
#include "random_access_reader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() / "disk_recovery_chunk_store_tests";
    const fs::path expected_root = fs::temp_directory_path();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "disk-a", ec);
    fs::create_directories(root / "disk-b", ec);
    if (ec) {
        std::cerr << "failed creating test directories: " << ec.message() << '\n';
        return 1;
    }

    std::vector<uint8_t> expected(1024);
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>((i * 37) & 0xFF);

    const fs::path index = root / "rescue.rci";
    const std::vector<recovery::ChunkDestinationSpec> destinations = {
        {root / "disk-a", 800}, {root / "disk-b", 400}
    };
    {
        recovery::ChunkedImageWriter writer;
        bool ok = writer.Create(index, expected.size(), 400, destinations) &&
                  writer.WriteAt(0, expected.data(), 137) &&
                  writer.WriteAt(137, expected.data() + 137, 700) &&
                  writer.WriteAt(837, expected.data() + 837, expected.size() - 837) &&
                  writer.Finalize();
        if (!ok) {
            std::cerr << "chunk writer failed with Windows error " << writer.LastErrorCode() << '\n';
            return 1;
        }
    }

    // A resumed writer must reuse the existing parts without truncating them.
    std::vector<uint8_t> replacement(93, 0xA5);
    std::copy(replacement.begin(), replacement.end(), expected.begin() + 376);
    {
        recovery::ChunkedImageWriter writer;
        if (!writer.OpenExisting(index, expected.size()) ||
            !writer.WriteAt(376, replacement.data(), replacement.size()) ||
            !writer.Finalize()) {
            std::cerr << "chunk resume failed with Windows error "
                      << writer.LastErrorCode() << '\n';
            return 1;
        }
    }

    recovery::RandomAccessReader reader;
    std::vector<uint8_t> actual(expected.size());
    if (!reader.Open(index) || !reader.ReadAt(0, actual.data(), actual.size()) || actual != expected) {
        std::cerr << "chunk reader did not reproduce the source bytes\n";
        return 1;
    }
    reader.Close();

    // A finalized part changed outside the writer must be rejected on resume.
    {
        std::fstream corrupt(root / "disk-a" / "rescue.part-000000.bin",
                             std::ios::in | std::ios::out | std::ios::binary);
        corrupt.seekp(0);
        corrupt.put(static_cast<char>(expected[0] ^ 0xFF));
    }
    {
        recovery::ChunkedImageWriter writer;
        if (writer.OpenExisting(index, expected.size()) || writer.LastErrorCode() != ERROR_CRC) {
            std::cerr << "chunk resume did not reject a corrupted finalized part\n";
            return 1;
        }
    }
    recovery::RandomAccessReader corrupted_reader;
    if (corrupted_reader.Open(index) || corrupted_reader.LastErrorCode() != ERROR_CRC) {
        std::cerr << "chunk reader did not reject a corrupted checksummed part\n";
        return 1;
    }
    std::ifstream index_file(index);
    const std::string index_text((std::istreambuf_iterator<char>(index_file)),
                                 std::istreambuf_iterator<char>());
    size_t part_count = 0, position = 0;
    while ((position = index_text.find("\npart ", position)) != std::string::npos) {
        ++part_count;
        position += 6;
    }
    if (part_count != 3 || index_text.find(" sha256 ") == std::string::npos) {
        std::cerr << "chunk index lacks expected parts or hashes\n";
        return 1;
    }
    index_file.close();

    // Lazy layout: parts exist only where non-zero data was written, the
    // aggregate capacity may be far below the image size, gaps read as zeros,
    // and a resumed writer keeps creating parts from the persisted destinations.
    {
        fs::create_directories(root / "lazy-a", ec);
        fs::create_directories(root / "lazy-b", ec);
        const fs::path lazy_index = root / "lazy.rci";
        const uint64_t lazy_total = 4096, lazy_chunk = 1024;
        std::vector<uint8_t> lazy_expected(lazy_total, 0);
        for (size_t i = 0; i < 300; ++i) lazy_expected[i] = static_cast<uint8_t>(i + 1);
        for (size_t i = 3072; i < 3072 + 50; ++i) lazy_expected[i] = 0x5A;
        const std::vector<recovery::ChunkDestinationSpec> lazy_destinations = {
            {root / "lazy-a", 1024}, {root / "lazy-b", 1024}
        };
        {
            recovery::ChunkedImageWriter writer;
            std::vector<uint8_t> zeros(2048, 0);
            bool ok = writer.Create(lazy_index, lazy_total, lazy_chunk, lazy_destinations, {}, true) &&
                      writer.WriteAt(0, lazy_expected.data(), 300) &&
                      writer.WriteAt(1024, zeros.data(), 2048) &&  // parts 1 and 2 stay absent
                      writer.WriteAt(3072, lazy_expected.data() + 3072, 100) &&
                      writer.Flush();
            if (!ok || writer.PartCount() != 2) {
                std::cerr << "lazy chunk writer failed with Windows error " << writer.LastErrorCode()
                          << " part count " << writer.PartCount() << '\n';
                return 1;
            }
        }
        if (!fs::exists(root / "lazy-a" / "lazy.part-000000.bin") ||
            !fs::exists(root / "lazy-b" / "lazy.part-000003.bin") ||
            fs::exists(root / "lazy-a" / "lazy.part-000001.bin") ||
            fs::exists(root / "lazy-b" / "lazy.part-000001.bin")) {
            std::cerr << "lazy parts were not placed by remaining capacity\n";
            return 1;
        }
        {
            recovery::RandomAccessReader reader;
            std::vector<uint8_t> actual(lazy_total, 0xFF);
            if (!reader.Open(lazy_index) || reader.Size() != lazy_total ||
                !reader.ReadAt(0, actual.data(), actual.size()) || actual != lazy_expected) {
                std::cerr << "lazy chunk reader did not reproduce zeros for absent parts\n";
                return 1;
            }
            std::vector<uint8_t> span(200);
            if (!reader.ReadAt(1000, span.data(), span.size()) ||
                !std::equal(span.begin(), span.end(), lazy_expected.begin() + 1000)) {
                std::cerr << "lazy chunk reader failed across a part/gap boundary\n";
                return 1;
            }
        }
        // Resume without destinations on the command line: the index carries
        // them. Writing non-zero data into an absent part creates it, but both
        // destinations are already at capacity, so the writer must refuse
        // rather than silently drop data.
        {
            recovery::ChunkedImageWriter writer;
            std::vector<uint8_t> more(10, 0x11);
            if (!writer.OpenExisting(lazy_index, lazy_total) || !writer.LazyParts() ||
                writer.WriteAt(2048, more.data(), more.size()) ||
                writer.LastErrorCode() != ERROR_DISK_FULL) {
                std::cerr << "lazy resume did not report a full destination set, error "
                          << writer.LastErrorCode() << '\n';
                return 1;
            }
        }
        // Resume with a new destination list: the part lands on the new disk.
        {
            fs::create_directories(root / "lazy-c", ec);
            recovery::ChunkedImageWriter writer;
            std::vector<uint8_t> more(10, 0x11);
            std::copy(more.begin(), more.end(), lazy_expected.begin() + 2048);
            const std::vector<recovery::ChunkDestinationSpec> extended = {
                {root / "lazy-a", 1024}, {root / "lazy-b", 1024}, {root / "lazy-c", 0}
            };
            if (!writer.OpenExisting(lazy_index, lazy_total, extended) ||
                !writer.WriteAt(2048, more.data(), more.size()) || !writer.Finalize() ||
                !fs::exists(root / "lazy-c" / "lazy.part-000002.bin")) {
                std::cerr << "lazy resume with extra destination failed, error "
                          << writer.LastErrorCode() << '\n';
                return 1;
            }
        }
        {
            recovery::RandomAccessReader reader;
            std::vector<uint8_t> actual(lazy_total, 0xFF);
            if (!reader.Open(lazy_index) || !reader.ReadAt(0, actual.data(), actual.size()) ||
                actual != lazy_expected) {
                std::cerr << "lazy chunk reader failed after finalize, error "
                          << reader.LastErrorCode() << '\n';
                return 1;
            }
        }
        std::ifstream lazy_file(lazy_index);
        const std::string lazy_text((std::istreambuf_iterator<char>(lazy_file)),
                                    std::istreambuf_iterator<char>());
        if (lazy_text.find("\nlazy 1\n") == std::string::npos ||
            lazy_text.find("\nchunk 1024\n") == std::string::npos ||
            lazy_text.find("\ndest ") == std::string::npos ||
            lazy_text.find("lazy-c") == std::string::npos) {
            std::cerr << "lazy index lacks layout or destination lines\n";
            return 1;
        }
    }

    const fs::path resolved = fs::weakly_canonical(root, ec);
    const fs::path resolved_temp = fs::weakly_canonical(expected_root, ec);
    if (ec || resolved.native().rfind(resolved_temp.native(), 0) != 0) {
        std::cerr << "refusing to remove test path outside temp\n";
        return 1;
    }
    fs::remove_all(resolved, ec);
    if (ec) {
        std::cerr << "failed removing test directory: " << ec.message() << '\n';
        return 1;
    }
    std::cout << "all chunk store tests passed\n";
    return 0;
}

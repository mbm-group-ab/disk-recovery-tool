// DR6: deterministic fuzzing of the files a crash, a bad disk or a user can leave behind.
// Mutated rescue maps and chunk indexes must never crash a tool, must never load into a state
// that claims more than the image holds, and reads outside the image must fail.
// The seed is fixed so a failure reproduces; set FUZZ_SEED / FUZZ_ROUNDS to explore further.
#include "chunked_image_writer.h"
#include "random_access_reader.h"
#include "rescue_map.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        if (failures < 20) std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

const char* const kTokens[] = {
    "0", "1", "-1", "18446744073709551615", "18446744073709551616", "99999999999999999999999",
    "9223372036854775808", "+", "-", "?", "*", "#total", "#source", "\"", "\\", " ", "\n",
    "#fingerprint \"", "\r\n", "\t", "\0", "4096", "1e9", "0x10",
};

std::string Mutate(std::string text, std::mt19937_64& rng) {
    const int edits = 1 + static_cast<int>(rng() % 4);
    for (int e = 0; e < edits; ++e) {
        const size_t pos = text.empty() ? 0 : rng() % (text.size() + 1);
        switch (rng() % 6) {
        case 0:  // flip a byte
            if (!text.empty()) text[pos % text.size()] = static_cast<char>(rng() & 0xFF);
            break;
        case 1:  // delete a run
            if (!text.empty()) text.erase(pos % text.size(), 1 + rng() % 16);
            break;
        case 2: {  // insert a hostile token
            const auto& token = kTokens[rng() % (sizeof(kTokens) / sizeof(kTokens[0]))];
            text.insert(pos, token);
            break;
        }
        case 3: {  // duplicate a line
            const size_t start = text.rfind('\n', pos == 0 ? 0 : pos - 1);
            const size_t from = start == std::string::npos ? 0 : start + 1;
            const size_t end = text.find('\n', from);
            const std::string line = text.substr(from, end == std::string::npos ? std::string::npos
                                                                              : end - from + 1);
            text.insert(from, line);
            break;
        }
        case 4:  // truncate (a crash mid-write)
            text.resize(pos);
            break;
        default:  // replace a digit run with a huge number
            for (size_t i = pos; i < text.size(); ++i) {
                if (text[i] >= '0' && text[i] <= '9') {
                    size_t j = i;
                    while (j < text.size() && text[j] >= '0' && text[j] <= '9') ++j;
                    text.replace(i, j - i, kTokens[3 + rng() % 4]);
                    break;
                }
            }
            break;
        }
    }
    return text;
}

// A loaded map must describe every byte of the image exactly once.
bool MapIsConsistent(const recovery::RescueMap& map, uint64_t total) {
    uint64_t covered = 0;
    for (auto status : {recovery::RangeStatus::NotTried, recovery::RangeStatus::Rescued,
                        recovery::RangeStatus::Bad, recovery::RangeStatus::Deferred}) {
        for (const auto& r : map.RangesWithStatus(status)) {
            if (r.size == 0 || r.offset > total || r.size > total - r.offset) return false;
            covered += r.size;
        }
    }
    return covered == total && map.TotalRescued() + map.TotalBad() <= total;
}

void FuzzRescueMaps(const fs::path& dir, uint64_t seed, int rounds) {
    constexpr uint64_t kTotal = 1 << 20;
    fs::create_directories(dir);
    const fs::path base = dir / "base.map";
    {
        recovery::RescueMap map(kTotal);
        map.ConfigureIdentity("file:source", "file:image", 512);
        map.SetSourceFingerprint("file:0001:00000002");
        map.MarkRescued(0, 4096);
        map.MarkBad(4096, 512);
        map.MarkDeferred(8192, 65536);
        map.MarkRescued(200000, 300000);
        if (!map.Save(base.string())) {
            Check(false, "base map saves");
            return;
        }
    }
    fs::remove(base.string() + ".bak");
    const std::string original = ReadText(base);

    // Hand-made hostile maps: overflow, wrap-around, gaps, overlaps.
    const std::vector<std::string> crafted = {
        "#total 1048576\n0 18446744073709551615 +\n",
        "#total 1048576\n0 1048576 +\n18446744073709551615 2 +\n",
        "#total 1048576\n0 1048575 +\n1048575 18446744073709551615 +\n",
        "#total 1048576\n0 99999999999999999999 +\n",
        "#total 1048576\n0 -1 +\n",
        "#total 1048576\n0 524288 +\n0 1048576 -\n",
        "#total 1048576\n0 524288 +\n524289 524287 +\n",
        "#total 1048576\n0 0 +\n0 1048576 +\n",
        "#total 1048576\n0 1048576 x\n",
        "#total 1048576\n0 1048576 + extra\n",
        "#total 18446744073709551615\n0 1048576 +\n",
        "#total 1048576\n",
        "",
    };
    const fs::path probe = dir / "probe.map";
    for (const auto& text : crafted) {
        WriteText(probe, text);
        recovery::RescueMap map(kTotal);
        Check(!map.Load(probe.string()), "hostile map is rejected: " + text.substr(0, 60));
    }

    std::mt19937_64 rng(seed);
    int loaded = 0;
    for (int round = 0; round < rounds; ++round) {
        const std::string text = Mutate(original, rng);
        WriteText(probe, text);
        recovery::RescueMap map(kTotal);
        if (round % 2) map.ConfigureIdentity("file:source", "file:image", 512);
        if (!map.Load(probe.string())) continue;
        ++loaded;
        Check(MapIsConsistent(map, kTotal), "mutated map " + std::to_string(round) +
                                                " loaded inconsistent ranges");
        // Whatever loads must also save and load again unchanged in meaning (sampled: Save
        // flushes to disk, which is slow).
        if (loaded % 16 != 1) continue;
        const fs::path again = dir / "again.map";
        fs::remove(again);
        fs::remove(again.string() + ".bak");
        Check(map.Save(again.string()), "loaded mutated map saves");
        recovery::RescueMap reread(kTotal);
        Check(reread.Load(again.string()) && reread.TotalRescued() == map.TotalRescued() &&
                  reread.TotalBad() == map.TotalBad(),
              "loaded mutated map round-trips");
    }
    std::cout << "rescue maps: " << rounds << " mutations, " << loaded << " still loadable\n";
}

void FuzzChunkIndexes(const fs::path& dir, uint64_t seed, int rounds) {
    constexpr size_t kSize = 3000;
    std::vector<uint8_t> data(kSize);
    for (size_t i = 0; i < kSize; ++i) data[i] = static_cast<uint8_t>((i * 131 + 7) & 0xFF);
    fs::create_directories(dir / "a");
    fs::create_directories(dir / "b");

    std::vector<std::string> originals;
    const std::vector<recovery::ChunkDestinationSpec> destinations = {{dir / "a", 0}, {dir / "b", 0}};
    for (bool lazy : {false, true}) {
        const fs::path index = dir / (lazy ? "lazy.rci" : "eager.rci");
        recovery::ChunkedImageWriter writer;
        const bool ok = writer.Create(index, kSize, 1024, destinations, {}, lazy) &&
                        writer.WriteAt(0, data.data(), 1500) &&
                        writer.WriteAt(2048, data.data() + 2048, kSize - 2048) && writer.Finalize();
        if (!ok) {
            Check(false, "chunked image for fuzzing is created");
            return;
        }
        originals.push_back(ReadText(index));
    }

    const fs::path probe = dir / "probe.rci";
    std::mt19937_64 rng(seed ^ 0x5DEECE66DULL);
    std::vector<uint8_t> buffer(4096);
    int opened = 0;
    for (int round = 0; round < rounds; ++round) {
        WriteText(probe, Mutate(originals[round % originals.size()], rng));
        recovery::RandomAccessReader reader;
        if (!reader.Open(probe)) continue;
        ++opened;
        const uint64_t size = reader.Size();
        for (int i = 0; i < 8 && size > 0; ++i) {
            const uint64_t offset = rng() % size;
            const size_t length = static_cast<size_t>(
                std::min<uint64_t>(1 + rng() % buffer.size(), size - offset));
            reader.ReadAt(offset, buffer.data(), length);  // may fail, must not crash
        }
        Check(!reader.ReadAt(size, buffer.data(), 1), "read at the end of a mutated index fails");
        if (size > 0) Check(!reader.ReadAt(size - 1, buffer.data(), 2), "read past the end fails");
        Check(!reader.ReadAt(UINT64_MAX - 1, buffer.data(), 16), "wrap-around read fails");
    }
    std::cout << "chunk indexes: " << rounds << " mutations, " << opened << " still openable\n";
}

}  // namespace

int main() {
    const char* seed_env = getenv("FUZZ_SEED");
    const char* rounds_env = getenv("FUZZ_ROUNDS");
    const uint64_t seed = seed_env ? strtoull(seed_env, nullptr, 10) : 20261123;
    const int rounds = rounds_env ? atoi(rounds_env) : 600;  // ~10 s; disk I/O bound

    const fs::path dir = fs::temp_directory_path() / "disk_recovery_fuzz_tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    FuzzRescueMaps(dir / "maps", seed, rounds);
    FuzzChunkIndexes(dir / "chunks", seed, rounds);

    fs::remove_all(dir, ec);
    if (failures == 0) std::cout << "all fuzz tests passed (seed " << seed << ")\n";
    else std::cerr << failures << " fuzz failures (seed " << seed << ")\n";
    return failures == 0 ? 0 : 1;
}

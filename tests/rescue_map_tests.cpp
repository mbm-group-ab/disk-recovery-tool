#include "rescue_map.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "disk_recovery_map_tests";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::string path = (dir / "image.map").string();

    recovery::RescueMap map(4096);
    map.MarkRescued(0, 1024);
    map.MarkBad(1024, 512);
    Check(map.Save(path), "initial map saves");
    Check(map.IsFullyRescued(0, 1024), "rescued range is reported rescued");
    Check(!map.IsFullyRescued(0, 2048), "mixed range is not reported rescued");
    Check(map.UnrescuedBytes(0, 2048) == 1024, "unrescued bytes are counted exactly");
    Check(map.NextPending(256, true).offset == 3840, "reverse pending selection starts at the end");
    Check(map.RangesWithStatus(recovery::RangeStatus::Bad).size() == 1,
          "bad ranges can be snapshotted for retry passes");
    Check(!map.IsFullyRescued(4090, 20), "out-of-bounds range is rejected");
    map.MarkDeferred(2048, 512);
    Check(map.RangesWithStatus(recovery::RangeStatus::Deferred).size() == 1,
          "fast-pass failures can be deferred for trimming");
    map.MarkNotTried(2048, 512);

    recovery::RescueMap loaded(4096);
    Check(loaded.Load(path), "valid map loads");
    Check(loaded.TotalRescued() == 1024, "rescued byte count survives reload");
    Check(loaded.TotalBad() == 512, "bad byte count survives reload");

    map.MarkRescued(1536, 2560);
    Check(map.Save(path), "replacement map saves and creates backup");
    {
        std::ofstream corrupt(path, std::ios::trunc);
        corrupt << "#total 4096\n0 4096 X\n";
    }
    recovery::RescueMap fallback(4096);
    Check(fallback.Load(path), "invalid primary map falls back to backup");
    Check(fallback.TotalRescued() == 1024, "backup contains prior valid generation");

    {
        std::ofstream invalid(path, std::ios::trunc);
        invalid << "#total 4096\n0 100 +\n200 3896 ?\n";
    }
    fs::remove(path + ".bak", ec);
    recovery::RescueMap rejected(4096);
    Check(!rejected.Load(path), "map with a coverage gap is rejected");

    const std::string identity_path = (dir / "identity.map").string();
    recovery::RescueMap identified(4096);
    identified.ConfigureIdentity("source-a", "output-a", 512);
    Check(identified.Save(identity_path), "identified map saves");
    Check(!identified.GenerationId().empty(), "identified map has a generation id");
    recovery::RescueMap same_identity(4096);
    same_identity.ConfigureIdentity("source-a", "output-a", 512);
    Check(same_identity.Load(identity_path), "matching map identity loads");
    Check(same_identity.GenerationId() == identified.GenerationId(),
          "generation id survives reload");
    recovery::RescueMap wrong_identity(4096);
    wrong_identity.ConfigureIdentity("source-b", "output-a", 512);
    Check(!wrong_identity.Load(identity_path), "mismatched source identity is rejected");

    fs::remove_all(dir, ec);
    if (failures == 0) std::cout << "all rescue map tests passed\n";
    return failures == 0 ? 0 : 1;
}

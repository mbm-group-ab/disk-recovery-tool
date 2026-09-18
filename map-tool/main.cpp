#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "rescue_map.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: rescue_map_tool <validate|repair> <image.map> <total-bytes>\n"
                     "       [--source ID --destination ID --sector N]\n";
        return 1;
    }
    const std::string operation = argv[1];
    const std::string path = argv[2];
    uint64_t total_size = 0;
    try {
        size_t end = 0;
        total_size = std::stoull(argv[3], &end);
        if (end != std::string(argv[3]).size() || total_size == 0) throw std::invalid_argument("size");
    } catch (...) {
        std::cerr << "invalid total byte count\n";
        return 1;
    }
    if (operation != "validate" && operation != "repair") {
        std::cerr << "operation must be validate or repair\n";
        return 1;
    }

    std::string source, destination;
    uint32_t sector = 0;
    for (int i = 4; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--source" && i + 1 < argc) source = argv[++i];
        else if (option == "--destination" && i + 1 < argc) destination = argv[++i];
        else if (option == "--sector" && i + 1 < argc) {
            try {
                const unsigned long parsed = std::stoul(argv[++i]);
                if (parsed > std::numeric_limits<uint32_t>::max()) throw std::out_of_range("sector");
                sector = static_cast<uint32_t>(parsed);
            } catch (...) {
                std::cerr << "invalid sector size\n";
                return 1;
            }
        } else {
            std::cerr << "unknown or incomplete option: " << option << '\n';
            return 1;
        }
    }
    if ((!source.empty() || !destination.empty() || sector != 0) &&
        (source.empty() || destination.empty() || sector == 0)) {
        std::cerr << "source, destination, and sector identity must be supplied together\n";
        return 1;
    }

    recovery::RescueMap map(total_size);
    if (!source.empty()) map.ConfigureIdentity(source, destination, sector);
    if (!map.Load(path)) {
        std::cerr << "no structurally valid matching map or backup\n";
        return 2;
    }
    std::cout << "valid map generation=" << (map.GenerationId().empty() ? "legacy" : map.GenerationId())
              << " source=" << (map.LoadedFromBackup() ? "backup" : "primary")
              << " rescued=" << map.TotalRescued() << " bad=" << map.TotalBad() << '\n';
    if (operation == "validate") return 0;

    if (map.LoadedFromBackup()) {
        std::error_code ec;
        const fs::path primary(path);
        if (fs::exists(primary, ec)) {
            fs::path quarantine(path + ".invalid");
            for (unsigned suffix = 1; fs::exists(quarantine, ec); ++suffix)
                quarantine = fs::path(path + ".invalid." + std::to_string(suffix));
            fs::rename(primary, quarantine, ec);
            if (ec) {
                std::cerr << "could not quarantine invalid primary: " << ec.message() << '\n';
                return 3;
            }
            std::cout << "quarantined invalid primary as " << quarantine.string() << '\n';
        }
    }
    if (!map.Save(path)) {
        std::cerr << "failed writing repaired map\n";
        return 3;
    }
    std::cout << "repair complete\n";
    return 0;
}

#include "sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <vector>

namespace recovery {

std::optional<std::string> Sha256File(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, result_size = 0;
    std::vector<uint8_t> object;
    std::array<uint8_t, 32> digest{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                &result_size, 0) >= 0;
    if (ok) {
        object.resize(object_size);
        ok = BCryptCreateHash(algorithm, &hash, object.data(), object_size,
                              nullptr, 0, 0) >= 0;
    }

    std::vector<uint8_t> buffer(4 * 1024 * 1024);
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), digest.size(), 0) >= 0;

    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!CloseHandle(file)) ok = false;
    if (!ok) return std::nullopt;

    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.resize(digest.size() * 2);
    for (size_t i = 0; i < digest.size(); ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    return output;
}

}  // namespace recovery

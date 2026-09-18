#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace recovery {

std::optional<std::string> Sha256File(const std::filesystem::path& path);

}  // namespace recovery

#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace migration {
bool durableWrite(std::filesystem::path const& file, std::string_view bytes);
std::optional<std::string> readFile(std::filesystem::path const& file);
std::string sha256(std::string_view bytes);
}

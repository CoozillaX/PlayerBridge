#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace profiles {
// Removes only the two root-level NetEase tags identified by the supplied test.go.
// Validates every length, nesting level and payload before returning any output.
std::optional<std::string> sanitizePlayerNbt(std::string_view data);
} // namespace profiles

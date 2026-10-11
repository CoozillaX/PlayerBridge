#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace profiles {
using Registry = nlohmann::json;
Registry emptyRegistry();
bool     validateRegistry(Registry const&);
bool mayClaim(Registry const&, std::string_view world, uint32_t uid, std::string_view sourceKey, std::string_view xuid);
Registry const* ownedProfile(Registry const&, std::string_view id, std::string_view xuid);
bool            safeProfileId(std::string_view id);
} // namespace profiles

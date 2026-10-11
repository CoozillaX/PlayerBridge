#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
class LevelStorage;

namespace profiles {
struct SourceRecord {
    std::string identityKey;
    std::string dataKey;
    std::string hash;
};

std::vector<SourceRecord>  findSources(LevelStorage&, uint32_t uid);
std::optional<std::string> identityDataKey(LevelStorage&, std::string const& identityKey, std::string const& msa);
std::optional<std::string> profileHash(LevelStorage&, std::string const& dataKey);
// Repairs only the original player record. The commit callback persists temporary
// digests before the world write; the caller removes them when marking it ready.
bool prepareProfile(LevelStorage&, nlohmann::json&, std::function<bool()> const& commit, std::string& error);
// Requires the old session to have exited. Writes identity indices only.
bool switchProfile(LevelStorage&, nlohmann::json const& account, std::string& error);
} // namespace profiles

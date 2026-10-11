#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
class LevelStorage;
namespace migration {
struct SourceRecord { std::string identityKey,dataKey,hash,neteaseUuid; };
std::vector<SourceRecord> findSources(LevelStorage&,uint32_t uid);
std::optional<std::string> identityDataKey(LevelStorage&,std::string const& identityKey,std::string const& msa);
std::optional<std::string> profileHash(LevelStorage&,std::string const& dataKey);
bool createProfileCopy(LevelStorage&,nlohmann::json& profile,std::filesystem::path const&,std::string& error);
// Requires the old session to have exited. Never deletes player records.
bool switchProfile(LevelStorage&,nlohmann::json const& account,std::string const& xuid,
                   std::filesystem::path const&,std::string& error);
}

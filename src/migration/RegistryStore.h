#pragma once
#include "migration/ProfileRegistry.h"
#include <filesystem>
#include <cstdint>
struct sqlite3;
namespace migration {
// Server-thread metadata cache backed by a transactional SQLite registry.
class RegistryStore {
public:
    RegistryStore() = default;
    ~RegistryStore();
    RegistryStore(RegistryStore const&) = delete;
    RegistryStore& operator=(RegistryStore const&) = delete;
    bool open(std::filesystem::path const&, std::string& error);
    bool load(std::filesystem::path const& legacyJson, Registry&, std::string& error);
    bool save(Registry const&, std::string& error);
private:
    sqlite3* mDb = nullptr;
    int64_t mGeneration = -1;
};
}

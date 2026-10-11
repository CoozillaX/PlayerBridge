#pragma once
#include "profiles/ProfileRegistry.h"
#include <filesystem>
#include <memory>

namespace ll::data {
class KeyValueDB;
}

namespace profiles {
// One server-thread writer; LL LevelDB lock excludes other database owners.
class RegistryStore {
public:
    RegistryStore();
    ~RegistryStore();
    RegistryStore(RegistryStore const&)            = delete;
    RegistryStore& operator=(RegistryStore const&) = delete;
    bool           open(std::filesystem::path const&, std::string& error);
    bool           load(Registry&, std::string& error);
    bool           save(Registry const&, std::string& error);

private:
    std::unique_ptr<ll::data::KeyValueDB> mDb;
    std::filesystem::path                 mCheckpoint;
    std::string                           mCommitted;
    uint64_t                              mGeneration = 0;
    bool                                  mLoaded     = false;
};
} // namespace profiles

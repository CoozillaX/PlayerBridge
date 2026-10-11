#include "profiles/RegistryStore.h"
#include "ll/api/data/KeyValueDB.h"
#include "profiles/Files.h"
#include <limits>
#include <stdexcept>

namespace profiles {
namespace {
constexpr auto RecordKey = "registry/v1";

std::string encode(Registry const& registry, uint64_t generation) {
    auto        payload = Registry::to_cbor(registry);
    std::string bytes(reinterpret_cast<char const*>(payload.data()), payload.size());
    Registry    envelope{
        {"format", 1},
        {"generation", generation},
        {"sha256", sha256(bytes)},
        {"registry", Registry::binary(payload)}
    };
    auto encoded = Registry::to_cbor(envelope);
    return {reinterpret_cast<char const*>(encoded.data()), encoded.size()};
}

Registry decode(std::string const& bytes, uint64_t& generation) {
    auto envelope = Registry::from_cbor(bytes);
    if (envelope.at("format") != 1 || !envelope.at("generation").is_number_unsigned()
        || !envelope.at("registry").is_binary()) {
        throw std::runtime_error("Unsupported LL registry format");
    }
    auto const& payload = envelope.at("registry").get_binary();
    std::string raw(reinterpret_cast<char const*>(payload.data()), payload.size());
    if (envelope.at("sha256") != sha256(raw)) {
        throw std::runtime_error("Registry checksum mismatch");
    }
    auto registry = Registry::from_cbor(payload);
    if (!validateRegistry(registry)) {
        throw std::runtime_error("Registry ownership validation failed");
    }
    generation = envelope.at("generation").get<uint64_t>();
    if (!generation) {
        throw std::runtime_error("Invalid registry generation");
    }
    return registry;
}

} // namespace

RegistryStore::RegistryStore()  = default;
RegistryStore::~RegistryStore() = default;

bool RegistryStore::open(std::filesystem::path const& path, std::string& error) {
    try {
        if (mDb) {
            throw std::runtime_error("Registry already open");
        }
        mCheckpoint = path.parent_path() / "profiles.commit.cbor";
        // Disable automatic repair: losing an ownership record must fail closed.
        mDb = std::make_unique<ll::data::KeyValueDB>(path, true, false, 10);
        return true;
    } catch (std::exception const& e) {
        error = e.what();
        return false;
    }
}

bool RegistryStore::load(Registry& result, std::string& error) {
    try {
        if (!mDb) {
            throw std::runtime_error("Registry not open");
        }
        if (std::filesystem::exists(mCheckpoint)) {
            auto bytes = readFile(mCheckpoint);
            if (!bytes) {
                throw std::runtime_error("Cannot read registry commit checkpoint");
            }
            auto registry = decode(*bytes, mGeneration);
            auto current  = mDb->get(RecordKey);
            if (current != bytes) {
                // LL WriteOptions has no synchronous-write switch. Replay the durable
                // commit checkpoint after an interrupted/asynchronous database write.
                ll::data::KeyValueDB::WriteBatch batch;
                batch.set(RecordKey, *bytes);
                if (!mDb->write(batch) || mDb->get(RecordKey) != bytes) {
                    throw std::runtime_error("Cannot recover committed LL registry");
                }
            }
            mCommitted = *bytes;
            mLoaded    = true;
            result     = std::move(registry);
            return true;
        }
        // Never silently initialize over existing metadata or fall back to stale imports.
        if (!mDb->empty()) {
            throw std::runtime_error("LL registry commit checkpoint missing");
        }
        auto imported = emptyRegistry();
        mLoaded       = true;
        if (!save(imported, error)) {
            mLoaded = false;
            return false;
        }
        result = std::move(imported);
        return true;
    } catch (std::exception const& e) {
        mLoaded = false;
        error   = e.what();
        return false;
    }
}

bool RegistryStore::save(Registry const& registry, std::string& error) {
    try {
        if (!mDb || !mLoaded) {
            throw std::runtime_error("Registry not loaded");
        }
        if (!validateRegistry(registry)) {
            throw std::runtime_error("Registry ownership validation failed");
        }
        if (mDb->get(RecordKey)
                != (mCommitted.empty() ? std::optional<std::string>{} : std::optional<std::string>{mCommitted})
            || (mCommitted.empty() ? std::filesystem::exists(mCheckpoint)
                                   : readFile(mCheckpoint) != std::optional<std::string>(mCommitted))) {
            throw std::runtime_error("Registry changed; stale update refused");
        }
        if (mGeneration == std::numeric_limits<uint64_t>::max()) {
            throw std::runtime_error("Registry generation exhausted");
        }
        auto bytes = encode(registry, mGeneration + 1);
        // The durable checkpoint is the commit decision. A later LL write failure
        // disables further changes; next startup replays this same complete snapshot.
        if (!durableWrite(mCheckpoint, bytes)) {
            throw std::runtime_error("Registry checkpoint write failed");
        }
        mCommitted = bytes;
        ++mGeneration;
        ll::data::KeyValueDB::WriteBatch batch;
        batch.set(RecordKey, bytes);
        if (!mDb->write(batch) || mDb->get(RecordKey) != std::optional<std::string>(bytes)) {
            mLoaded = false;
            throw std::runtime_error("LL registry write failed; committed checkpoint awaits recovery");
        }
        return true;
    } catch (std::exception const& e) {
        error = e.what();
        return false;
    }
}
} // namespace profiles

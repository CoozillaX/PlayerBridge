#include "ll/api/data/KeyValueDB.h"
#include "profiles/Files.h"
#include "profiles/Identity.h"
#include "profiles/RegistryStore.h"
#include <filesystem>
#include <stdexcept>

namespace fixture {
namespace {
using namespace profiles;

void require(bool value, std::string const& text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

Registry account(std::string id) {
    return {
        {"world", "fixture"},
        {"uuid", "uuid-" + id},
        {"msa_id", id},
        {"self_signed_id", ""},
        {"identity_key", "player_" + id},
        {"default_key", "player_server_original-" + id},
        {"active_profile", "default"},
        {"pending", nullptr}
    };
}

Registry sample() {
    auto r               = emptyRegistry();
    r["accounts"]["111"] = account("111");
    r["accounts"]["222"] = account("222");
    auto id              = makeChallenge();
    r["profiles"].push_back(
        {{"id", id},
         {"owner", "111"},
         {"world", "fixture"},
         {"uid", 4000000001u},
         {"name", "网易角色"},
         {"source_identity_key", "player_netease"},
         {"source_data_key", "player_netease"},
         {"source_hash", "source-hash"},
         {"expected_hash", "copy-hash"},
         {"data_key", "player_server_profiles_" + id},
         {"status", "ready"},
         {"netease_uuid", "00000000-0000-4000-8000-000000000001"},
         {"error", ""},
         {"created_at", 123}}
    );
    r["accounts"]["111"]["pending"] = {
        {"id", makeChallenge()},
        {"profile_id", id},
        {"previous_key", "player_server_original-111"},
        {"next_key", "player_server_profiles_" + id},
        {"status", "pending"},
        {"created_at", 124}
    };
    return r;
}

} // namespace

bool runRegistryTests(std::filesystem::path const& base, std::string& error) {
    try {
        auto root = base / ("registry-tests-" + makeChallenge());
        std::filesystem::create_directories(root);
        auto db = root / "profiles.leveldb", checkpoint = root / "profiles.commit.cbor";
        auto original = sample();
        require(validateRegistry(original), "valid sample");
        {
            RegistryStore store;
            Registry      loaded;
            require(store.open(db, error) && store.load(loaded, error), error);
            require(loaded == emptyRegistry(), "fresh LL registry starts empty");
            require(store.save(original, error), error);
            require(
                store.load(loaded, error) && loaded == original,
                "LL registry preserves all fields and pending switch"
            );
            RegistryStore duplicate;
            require(!duplicate.open(db, error), "LL database lock excludes another writer");
            auto invalid = original, copy = invalid["profiles"][0];
            auto id                 = makeChallenge();
            copy["id"]              = id;
            copy["owner"]           = "222";
            copy["data_key"]        = "player_server_profiles_" + id;
            copy["source_data_key"] = "player_other";
            invalid["profiles"].push_back(copy);
            auto before = readFile(checkpoint);
            require(!store.save(invalid, error), "duplicate UID owner refused");
            require(readFile(checkpoint) == before, "rejected write keeps commit unchanged");
            invalid                                      = original;
            invalid["accounts"]["222"]["self_signed_id"] = "111";
            require(!store.save(invalid, error), "duplicate identity alias refused");
            invalid                                      = original;
            invalid["accounts"]["222"]["active_profile"] = original["profiles"][0]["id"];
            require(!store.save(invalid, error), "foreign active profile refused");
            auto complete                                 = original;
            complete["accounts"]["111"]["active_profile"] = complete["profiles"][0]["id"];
            complete["accounts"]["111"]["last_switch"]    = complete["accounts"]["111"]["pending"];
            complete["accounts"]["111"]["pending"]        = nullptr;
            require(store.save(complete, error), error);
            auto committedBytes = readFile(checkpoint);
            require(durableWrite(checkpoint, "external-change"), "inject external checkpoint change");
            require(!store.save(complete, error), "stale checkpoint update refused");
            require(durableWrite(checkpoint, *committedBytes), "restore committed checkpoint");
            auto blockedTemp  = checkpoint;
            blockedTemp      += L".tmp";
            std::filesystem::create_directory(blockedTemp);
            require(!store.save(complete, error), "checkpoint I/O failure refuses commit");
            require(readFile(checkpoint) == committedBytes, "I/O failure preserves committed state");
            std::filesystem::remove(blockedTemp);
            original = complete;
        }
        // Simulate loss of the asynchronous LL write, retaining the durable commit decision.
        {
            ll::data::KeyValueDB raw(db, true, false, 10);
            require(raw.set("registry/v1", "interrupted-write"), "inject interrupted write");
        }
        {
            RegistryStore store;
            Registry      loaded;
            require(store.open(db, error) && store.load(loaded, error), error);
            require(loaded == original, "checkpoint replays full committed state on reopen");
            auto checkpointBytes = readFile(checkpoint);
            require(bool(checkpointBytes), "checkpoint exists");
            require(durableWrite(checkpoint, "broken"), "inject corrupt checkpoint");
            require(!store.load(loaded, error), "corrupt checkpoint refuses reinitialization");
            require(durableWrite(checkpoint, *checkpointBytes), "restore fixture checkpoint");
        }
        auto missing = root / "missing-checkpoint";
        std::filesystem::create_directories(missing);
        {
            ll::data::KeyValueDB raw(missing / "profiles.leveldb", true, false, 10);
            require(raw.set("registry/v1", "unknown"), "unknown database fixture");
        }
        {
            RegistryStore store;
            Registry      loaded;
            require(
                store.open(missing / "profiles.leveldb", error) && !store.load(loaded, error),
                "missing checkpoint refuses initialization"
            );
        }
        return true;
    } catch (std::exception const& e) {
        error = e.what();
        return false;
    }
}
} // namespace fixture

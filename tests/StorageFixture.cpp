#include "mc/world/level/storage/PlayerDataSystem.h"
#include "mc/world/level/storage/PlayerStorageIds.h"
// A separate test-only DLL. Never packaged or installed with the production plugin.
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/storage/LevelStorage.h"
#include "mc/world/level/storage/LevelStorageWriteBatch.h"
#include "profiles/Files.h"
#include "profiles/Identity.h"
#include "profiles/NbtSanitizer.h"
#include "profiles/ProfileRegistry.h"
#include "profiles/Storage.h"
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fixture {
bool runRegistryTests(std::filesystem::path const&, std::string& error);
using Json              = nlohmann::json;
constexpr auto Category = DBHelpers::Category::Player;

void check(bool value, std::string const& text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

void integer(std::string& data, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        data += static_cast<char>(value >> (i * 8));
    }
}

void name(std::string& data, std::string_view text) {
    integer(data, text.size(), 2);
    data += text;
}

std::string playerData(int xp, int64_t uid, bool netease = false) {
    std::string data;
    data += char(10);
    name(data, "");
    data += char(4);
    name(data, "UniqueID");
    integer(data, uid, 8);
    data += char(9);
    name(data, "Inventory");
    integer(data, 10, 1);
    integer(data, 0, 4);
    data += char(3);
    name(data, "XpLevel");
    integer(data, xp, 4);
    if (netease) {
        data += char(8);
        name(data, "scriptData");
        integer(data, 3, 4);
        data += "abc";
        data += char(8);
        name(data, "saveLongString");
        name(data, "original-must-stay");
    }
    data += char(0);
    return data;
}

std::string read(LevelStorage& storage, std::string const& key) {
    std::string value;
    check(storage.loadData(key, value, Category), "storage record missing");
    return value;
}

void write(LevelStorage& storage, std::string const& key, std::string const& bytes) {
    LevelStorageWriteBatch batch;
    batch.putKey(key, bytes, Category);
    storage.saveData(batch);
    storage.flushToPermanentStorage();
    check(read(storage, key) == bytes, "storage write not visible");
}

class TestMod {
    std::string         stage = "initialization";
    ll::mod::NativeMod& self  = *ll::mod::NativeMod::current();

public:
    static TestMod& getInstance() {
        static TestMod test;
        return test;
    }

    bool load() {
        return true;
    }

    bool disable() {
        return true;
    }

    bool enable() {
        auto& registryCommand = ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand(
            "profiledbtest",
            "Run LL registry database tests",
            CommandPermissionLevel::Any
        );
        registryCommand.overload().execute([this](CommandOrigin const& origin, CommandOutput& output) {
            auto level = ll::service::getLevel();
            if (origin.getEntity() || !level
                || level->getLevelStorage().getFullPath().value.find("profiles-test") == std::string::npos) {
                output.error("Registry fixture requires console and isolated test world");
                return;
            }
            bool online = false;
            level->forEachPlayer([&](Player&) {
                online = true;
                return true;
            });
            if (online) {
                output.error("Registry fixture requires an empty server");
                return;
            }
            std::string error;
            if (runRegistryTests(self.getDataDir(), error)) {
                output.success("LL REGISTRY FIXTURE PASSED");
            } else {
                output.error("LL REGISTRY FIXTURE FAILED: {}", error);
            }
        });
        auto& loadFixture = ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand(
            "profileloadfixture",
            "Exercise real pre-load hook on fixture",
            CommandPermissionLevel::Any
        );
        loadFixture.overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
            auto level = ll::service::getLevel();
            if (origin.getEntity() || !level
                || level->getLevelStorage().getFullPath().value.find("profiles-test") == std::string::npos) {
                return;
            }
            alignas(PlayerStorageIds) std::byte buffer[sizeof(PlayerStorageIds)];
            auto*                               ids = new (buffer) PlayerStorageIds{};
            ids->MsaId                              = std::string("11111111-1111-3111-8111-111111111111");
            ids->SelfSignedId                       = std::string("fixture-uuid");
            auto data = PlayerDataSystem::loadPlayerData(level->getLevelStorage(), *ids, false, false);
            std::destroy_at(&ids->MsaId.get());
            std::destroy_at(&ids->GuestId.get());
            std::destroy_at(&ids->PlatformId.get());
            std::destroy_at(&ids->PlatformOnlineId.get());
            std::destroy_at(&ids->PlatformOfflineId.get());
            std::destroy_at(&ids->SelfSignedId.get());
            std::destroy_at(&ids->RandomClientId.get());
            if (data && data->contains("Inventory", Tag::List)) {
                output.success("PRELOAD FIXTURE PASSED");
            } else {
                output.error("PRELOAD FIXTURE FAILED");
            }
        });

        auto& command = ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand(
            "profiletest",
            "Run isolated profile storage fixture",
            CommandPermissionLevel::Any
        );
        command.overload().execute([this](CommandOrigin const& origin, CommandOutput& output) {
            try {
                check(!origin.getEntity(), "fixture can only be run from console");
                auto level = ll::service::getLevel();
                check(bool(level), "no level");
                auto& storage = level->getLevelStorage();
                auto  world   = storage.getFullPath().value;
                check(world.find("profiles-test") != std::string::npos, "fixture refused outside dedicated test world");
                bool occupied = false;
                level->forEachPlayer([&](Player&) {
                    occupied = true;
                    return false;
                });
                check(!occupied, "test world must be empty");
                run(storage, world);
                output.success("PROFILE STORAGE FIXTURE PASSED");
            } catch (std::exception const& e) {
                output.error(std::string("PROFILE STORAGE FIXTURE FAILED at ") + stage + ": " + e.what());
            }
        });
        return true;
    }

    void run(LevelStorage& storage, std::string const& world) {
        stage                             = "alias-only sources";
        auto               source         = playerData(23, 9999, true);
        auto               aliasEncrypted = profiles::encryptUid(42u);
        std::ostringstream aliasHex;
        aliasHex << std::hex << std::setw(8) << std::setfill('0') << aliasEncrypted;
        std::string netUuid   = "00000000-0000-4000-8000-0000" + aliasHex.str(),
                    aliasData = "player_server_fixture_alias_netease";
        CompoundTag alias;
        alias["MsaId"]        = netUuid;
        alias["SelfSignedId"] = std::string("fixture-role-alias");
        alias["ServerId"]     = aliasData;
        write(storage, aliasData, source);
        write(storage, "player_fixture-role-alias", alias.toBinaryNbt());
        write(storage, "player_fixture-role-alias-duplicate", alias.toBinaryNbt());
        auto aliasSources = profiles::findSources(storage, 42u);
        check(aliasSources.empty(), "historical role aliases must not be offered as account profiles");
        check(read(storage, aliasData) == source, "source lookup changed original source");
        stage    = "profile fixture";
        auto dir = self.getDataDir(), statePath = dir / "fixture-optional-id.json";
        if (auto previous = profiles::readFile(statePath)) {
            auto        state = Json::parse(*previous);
            auto const& r     = state.at("registry");
            check(profiles::validateRegistry(r), "persistent registry invalid");
            auto const& a = r["accounts"]["900001"];
            check(
                profiles::identityDataKey(
                    storage,
                    "player_11111111-1111-3111-8111-111111111111",
                    "11111111-1111-3111-8111-111111111111"
                ) == state["selected_key"].get<std::string>(),
                "selected profile lost after restart"
            );
            check(
                profiles::sha256(read(storage, state["source_key"])) == state["source_hash"].get<std::string>(),
                "repaired NetEase progress lost"
            );
            check(
                profiles::sha256(read(storage, a["default_key"])) == state["default_hash"].get<std::string>(),
                "original international progress lost"
            );
            check(
                profiles::sha256(read(storage, state["selected_key"])) == state["updated_hash"].get<std::string>(),
                "playable profile progress lost"
            );
            auto verified = profiles::findSources(storage, 4000000001u);
            check(verified.size() == 1, "original source record disappeared");
            state["restart_verified"] = true;
            check(profiles::durableWrite(statePath, state.dump(2)), "write restart evidence");
            return;
        }
        auto               suffix = profiles::encryptUid(4000000001u);
        std::ostringstream hex;
        hex << std::hex << std::setw(8) << std::setfill('0') << suffix;
        auto        sourceKey   = "player_00000000-0000-4000-8000-0000" + hex.str();
        auto        original    = playerData(7, 8888);
        std::string defaultKey  = "player_server_fixture_original",
                    identityKey = "player_11111111-1111-3111-8111-111111111111";
        if (storage.hasKey(sourceKey, Category) || storage.hasKey(defaultKey, Category)) {
            check(
                read(storage, sourceKey) == source && read(storage, defaultKey) == original,
                "incomplete fixture seed differs"
            );
        } else {
            write(storage, sourceKey, source);
            write(storage, defaultKey, original);
        }
        stage = "construct identity";
        CompoundTag index;
        index["MsaId"]          = std::string("11111111-1111-3111-8111-111111111111");
        index["SelfSignedId"]   = std::string("fixture-uuid");
        index["ServerId"]       = defaultKey;
        index["UnrelatedField"] = std::string("must-stay");
        stage                   = "serialize identity";
        write(storage, identityKey, index.toBinaryNbt());
        write(storage, "player_fixture-uuid", index.toBinaryNbt());
        stage        = "discover sources";
        auto sources = profiles::findSources(storage, 4000000001u);
        check(sources.size() == 1, "source discovery failed");
        auto r                  = profiles::emptyRegistry();
        r["accounts"]["900001"] = {
            {"world", world},
            {"uuid", "fixture-uuid"},
            {"msa_id", "11111111-1111-3111-8111-111111111111"},
            {"self_signed_id", "fixture-uuid"},
            {"identity_key", identityKey},
            {"default_key", defaultKey},
            {"active_profile", "default"},
            {"pending", nullptr}
        };
        auto id = profiles::makeChallenge();
        Json p{
            {"id", id},
            {"owner", "900001"},
            {"uid", 4000000001u},
            {"world", world},
            {"name", "fixture"},
            {"source_identity_key", sourceKey},
            {"source_hash", profiles::sha256(source)},
            {"expected_hash", ""},
            {"data_key", sourceKey},
            {"status", "adding"}
        };
        r["profiles"].push_back(p);
        check(profiles::validateRegistry(r), "ownership reservation invalid");
        auto&       profile = r["profiles"][0];
        std::string error;
        stage            = "repair original source";
        auto interrupted = profile;
        check(
            !profiles::prepareProfile(
                storage,
                interrupted,
                [] {
                    return false;
                },
                error
            ),
            "failed commit accepted"
        );
        check(read(storage, sourceKey) == source, "failed commit changed original player data");
        check(interrupted["expected_hash"] == "", "failed commit retained an uncommitted digest");
        auto changed           = profile;
        changed["source_hash"] = "unexpected";
        check(
            !profiles::prepareProfile(
                storage,
                changed,
                [] {
                    return true;
                },
                error
            ),
            "changed source accepted"
        );
        check(read(storage, sourceKey) == source, "rejected source was modified");
        check(
            profiles::prepareProfile(
                storage,
                profile,
                [] {
                    return true;
                },
                error
            ),
            error
        );
        check(
            profiles::prepareProfile(
                storage,
                profile,
                [] {
                    return true;
                },
                error
            ),
            "interrupted addition is not idempotent"
        );
        profile["status"] = "ready";
        profile.erase("source_hash");
        profile.erase("expected_hash");
        check(read(storage, sourceKey) == *profiles::sanitizePlayerNbt(source), "repair changed unrelated bytes");
        check(!storage.hasKey("player_server_profiles_" + id, Category), "duplicate player record created");
        check(
            !std::filesystem::exists(dir / "originals") && !std::filesystem::exists(dir / "switches"),
            "NBT backup created"
        );
        auto selected = profile["data_key"].get<std::string>();
        check(read(storage, selected) != source, "NetEase foreign tags not cleaned");
        stage       = "parse repaired original";
        auto parsed = CompoundTag::fromBinaryNbt(read(storage, selected));
        check(bool(parsed), "original NBT invalid");
        check(parsed->at("UniqueID").get<Int64Tag>().data == 9999, "actor ID changed during repair");
        check(
            !parsed->contains("scriptData") && !parsed->contains("saveLongString"),
            "foreign fields retained in original"
        );
        check(!profiles::mayClaim(r, world, 4000000001u, sourceKey, "900002"), "second owner accepted");
        auto& a        = r["accounts"]["900001"];
        auto  switchTo = [&](std::string const& profileId, std::string const& next) {
            auto current = profiles::identityDataKey(storage, identityKey, "11111111-1111-3111-8111-111111111111");
            check(current.has_value(), "identity missing");
            a["pending"] = {
                {"id", profiles::makeChallenge()},
                {"profile_id", profileId},
                {"previous_key", *current},
                {"next_key", next},
                {"status", "applying"}
            };
            check(profiles::validateRegistry(r), "switch ownership invalid");
            stage = "switch profile";
            check(profiles::switchProfile(storage, a, error), error);
            check(profiles::switchProfile(storage, a, error), "committed switch recovery failed");
            a["active_profile"]       = profileId;
            a["pending"]              = nullptr;
            auto expectedIndex        = index;
            expectedIndex["ServerId"] = next;
            check(read(storage, identityKey) == expectedIndex.toBinaryNbt(), "switch changed other identity fields");
            check(
                profiles::identityDataKey(storage, identityKey, "11111111-1111-3111-8111-111111111111") == next,
                "index not switched"
            );
        };
        switchTo(id, selected);
        auto updated = playerData(43, 9999);
        write(storage, selected, updated); // Simulate normal engine saving to selected slot.
        switchTo("default", defaultKey);
        check(read(storage, defaultKey) == original, "original international slot overwritten");
        switchTo(id, selected);
        check(read(storage, selected) == updated, "NetEase playable progress reset on repeat switch");
        // Incoming authenticated storage IDs can omit the optional self-signed ID.
        a["self_signed_id"] = "";
        switchTo("default", defaultKey);
        switchTo(id, selected);
        a["self_signed_id"] = "fixture-uuid";
        check(selected == sourceKey && read(storage, sourceKey) == updated, "original NetEase progress not used");
        check(
            !std::filesystem::exists(dir / "originals") && !std::filesystem::exists(dir / "switches"),
            "switch saved NBT backups"
        );
        auto indexBefore = read(storage, identityKey);
        a["pending"]     = {
            {"id", profiles::makeChallenge()},
            {"profile_id", "default"},
            {"previous_key", selected},
            {"next_key", "player_server_missing"},
            {"status", "applying"}
        };
        check(!profiles::switchProfile(storage, a, error), "missing target was accepted");
        check(read(storage, identityKey) == indexBefore, "failed switch changed identity");
        a["pending"] = nullptr;
        Json state{
            {"registry", r},
            {"source_key", sourceKey},
            {"source_hash", profiles::sha256(updated)},
            {"selected_key", selected},
            {"default_hash", profiles::sha256(original)},
            {"updated_hash", profiles::sha256(updated)},
            {"restart_verified", false}
        };
        check(profiles::durableWrite(statePath, state.dump(2)), "write fixture journal");
    }
};
} // namespace fixture

LL_REGISTER_MOD(fixture::TestMod, fixture::TestMod::getInstance());

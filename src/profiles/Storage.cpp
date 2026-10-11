#include "profiles/Storage.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/world/level/storage/LevelStorage.h"
#include "mc/world/level/storage/LevelStorageWriteBatch.h"
#include "profiles/Files.h"
#include "profiles/Identity.h"
#include "profiles/NbtSanitizer.h"
#include "profiles/ProfileRegistry.h"
#include <algorithm>
#include <cstdio>
#include <set>

namespace profiles {
namespace {
constexpr auto Category = DBHelpers::Category::Player;

std::string field(CompoundTag const& tag, std::string_view name) {
    return tag.contains(name, Tag::String) ? std::string(tag[name].get<StringTag>()) : std::string{};
}

bool isPlayer(CompoundTag const& tag) {
    return tag.contains("UniqueID", Tag::Int64) && tag.contains("Inventory", Tag::List);
}

std::optional<std::string> load(LevelStorage& storage, std::string const& key) {
    std::string bytes;
    if (!storage.loadData(key, bytes, Category) || bytes.size() > 32 * 1024 * 1024) {
        return {};
    }
    return bytes;
}

std::optional<std::string> canonical(std::string_view data) {
    auto clean = sanitizePlayerNbt(data);
    if (!clean) {
        return {};
    }
    auto tag = CompoundTag::fromBinaryNbt(*clean);
    if (!tag || !isPlayer(*tag)) {
        return {};
    }
    return tag->toBinaryNbt();
}
} // namespace

std::vector<SourceRecord> findSources(LevelStorage& storage, uint32_t uid) {
    // Only the account's authoritative identity keys are eligible. Historical role
    // aliases can have the same MsaId and must not become selectable profiles.
    auto encrypted = encryptUid(uid);
    auto reversed =
        ((encrypted & 0xff) << 24) | ((encrypted & 0xff00) << 8) | ((encrypted >> 8) & 0xff00) | (encrypted >> 24);
    std::vector<std::string> candidates;
    for (auto tail : {reversed, encrypted}) {
        char hex[9]{};
        std::snprintf(hex, sizeof(hex), "%08x", tail);
        auto key = std::string("player_00000000-0000-4000-8000-0000") + hex;
        if (std::find(candidates.begin(), candidates.end(), key) == candidates.end()) {
            candidates.push_back(key);
        }
    }
    std::vector<SourceRecord> result;
    std::set<std::string>     dataKeys;
    for (auto const& identity : candidates) {
        auto bytes = load(storage, identity);
        if (!bytes) {
            continue;
        }
        auto clean = sanitizePlayerNbt(*bytes);
        if (!clean) {
            continue;
        }
        auto tag = CompoundTag::fromBinaryNbt(*clean);
        if (!tag) {
            continue;
        }
        auto neteaseUuid = field(*tag, "MsaId");
        if (!uuidMatchesUid(neteaseUuid, uid)) {
            neteaseUuid = identity.substr(7);
        }
        if (!uuidMatchesUid(neteaseUuid, uid)) {
            continue;
        }
        auto key = field(*tag, "ServerId");
        if (key.empty()) {
            key = identity;
        }
        if (!key.starts_with("player_") || key.starts_with("player_server_profiles_")) {
            continue;
        }
        auto data = load(storage, key);
        if (!data || !canonical(*data)) {
            continue;
        }
        auto hash = sha256(*data);
        if (!hash.empty() && dataKeys.insert(key).second) {
            result.push_back({identity, key, hash, neteaseUuid});
        }
        if (!result.empty()) {
            break; // Prefer the exported account key, never historical alternatives.
        }
    }
    return result;
}

std::optional<std::string> identityDataKey(LevelStorage& storage, std::string const& key, std::string const& xuid) {
    if (key != "player_" + xuid || xuid.empty()) {
        return {};
    }
    auto bytes = load(storage, key);
    if (!bytes) {
        return {};
    }
    auto tag = CompoundTag::fromBinaryNbt(*bytes);
    if (!tag || field(*tag, "MsaId") != xuid) {
        return {};
    }
    auto dataKey = field(*tag, "ServerId");
    if (!dataKey.starts_with("player_server_")) {
        return {};
    }
    return dataKey;
}

std::optional<std::string> profileHash(LevelStorage& storage, std::string const& key) {
    auto data = load(storage, key);
    if (!data || !canonical(*data)) {
        return {};
    }
    auto hash = sha256(*data);
    return hash.empty() ? std::optional<std::string>{} : hash;
}

bool createProfileCopy(LevelStorage& storage, nlohmann::json& p, std::filesystem::path const& dir, std::string& error) {
    auto refuse = [&](std::string text) {
        error = std::move(text);
        return false;
    };
    auto id = p.at("id").get<std::string>(), sourceKey = p.at("source_data_key").get<std::string>();
    auto sourceIdentity = p.at("source_identity_key").get<std::string>(), key = p.at("data_key").get<std::string>();
    auto neteaseUuid = p.value("netease_uuid", sourceIdentity.substr(7));
    if (!safeProfileId(id) || key != "player_server_profiles_" + id || key == sourceKey
        || !sourceKey.starts_with("player_") || !sourceIdentity.starts_with("player_")
        || sourceKey.starts_with("player_server_profiles_")
        || !uuidMatchesUid(neteaseUuid, p.at("uid").get<uint32_t>())) {
        return refuse("档案标识不匹配，未修改存档。");
    }
    std::string step = "load source";
    try {
        auto source = load(storage, sourceKey), originalIdentity = load(storage, sourceIdentity);
        if (!source || !originalIdentity || sha256(*source) != p.at("source_hash").get<std::string>()) {
            return refuse("网易原始档案发生变化或无法读取。");
        }
        if (sourceIdentity != "player_" + neteaseUuid) {
            auto clean = sanitizePlayerNbt(*originalIdentity);
            if (!clean) {
                return refuse("网易身份索引无法解析。");
            }
            auto index = CompoundTag::fromBinaryNbt(*clean);
            if (!index || field(*index, "MsaId") != neteaseUuid || field(*index, "ServerId") != sourceKey) {
                return refuse("网易角色索引已变化，未修改档案。");
            }
        }
        step      = "canonicalize source";
        auto data = canonical(*source);
        if (!data) {
            return refuse("网易档案无法安全解析。");
        }
        auto expected = sha256(*data);
        if (expected.empty()) {
            return refuse("档案校验失败。");
        }
        if (!p.at("expected_hash").get<std::string>().empty() && p.at("expected_hash").get<std::string>() != expected) {
            return refuse("待添加档案与备份不一致。");
        }
        step              = "prepare backup";
        auto backup       = dir / "originals" / id;
        auto manifestPath = backup / "manifest.json";
        if (std::filesystem::exists(manifestPath)) {
            auto text = readFile(manifestPath), saved = readFile(backup / "source.bin"),
                 savedIdentity = readFile(backup / "identity.bin");
            auto manifest      = text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json{};
            if (!manifest.is_object() || manifest.value("id", "") != id || manifest.value("data_key", "") != key
                || manifest.value("owner", "") != p.at("owner").get<std::string>()
                || manifest.value("world", "") != p.at("world").get<std::string>()
                || manifest.value("source_data_key", "") != sourceKey
                || manifest.value("source_identity_key", "") != sourceIdentity || !saved || !savedIdentity
                || sha256(*saved) != p.at("source_hash").get<std::string>()
                || sha256(*savedIdentity) != manifest.value("identity_hash", "")
                || manifest.value("expected_hash", "") != expected) {
                return refuse("原始档案备份不完整，已保留记录，请联系管理员。");
            }
        } else {
            auto manifest             = p;
            manifest["expected_hash"] = expected;
            manifest["identity_hash"] = sha256(*originalIdentity);
            if (!durableWrite(backup / "source.bin", *source)
                || !durableWrite(backup / "identity.bin", *originalIdentity)
                || !durableWrite(manifestPath, manifest.dump(2))) {
                return refuse("原始档案备份失败，未修改存档。");
            }
        }
        step = "write copy";
        if (auto existing = load(storage, key)) {
            if (sha256(*existing) != expected) {
                return refuse("可用档案已存在且内容不同，禁止覆盖。");
            }
        } else {
            LevelStorageWriteBatch batch;
            batch.putKey(key, *data, Category);
            storage.saveData(batch);
            storage.flushToPermanentStorage();
        }
        auto actual = load(storage, key), unchanged = load(storage, sourceKey),
             unchangedIdentity = load(storage, sourceIdentity);
        if (!actual || sha256(*actual) != expected || !unchanged || *unchanged != *source || !unchangedIdentity
            || *unchangedIdentity != *originalIdentity) {
            return refuse("添加档案回读检查失败，请联系管理员。");
        }
        p["expected_hash"] = expected;
        return true;
    } catch (std::exception const& e) {
        return refuse(step + ": " + e.what());
    }
}

bool switchProfile(
    LevelStorage&                storage,
    nlohmann::json const&        a,
    std::string const&           xuid,
    std::filesystem::path const& dir,
    std::string&                 error
) {
    auto refuse = [&](std::string text) {
        error = std::move(text);
        return false;
    };
    auto const& pending = a.at("pending");
    auto        id = pending.at("id").get<std::string>(), key = a.at("identity_key").get<std::string>();
    auto previous = pending.at("previous_key").get<std::string>(), next = pending.at("next_key").get<std::string>();
    auto msa        = a.at("msa_id").get<std::string>();
    auto selfSigned = a.at("self_signed_id").get<std::string>();
    auto alias      = selfSigned.empty() ? key : "player_" + selfSigned;
    if (!safeProfileId(id) || key != "player_" + msa || msa.empty() || !previous.starts_with("player_server_")
        || !next.starts_with("player_server_") || previous == next) {
        return refuse("档案切换标识无效。");
    }
    storage.flushToPermanentStorage();
    auto current = identityDataKey(storage, key, msa);
    if (!current) {
        return refuse("国际服身份索引无法读取。");
    }
    if (*current == next) {
        auto aliasBytes = load(storage, alias);
        if (!aliasBytes || !profileHash(storage, next)) {
            return refuse("恢复切换时目标档案或身份别名无法读取。");
        }
        auto aliasTag = CompoundTag::fromBinaryNbt(*aliasBytes);
        if (!aliasTag || field(*aliasTag, "MsaId") != msa || field(*aliasTag, "ServerId") != next) {
            return refuse("恢复切换时身份别名不一致。");
        }
        return true;
    }
    if (*current != previous) {
        return refuse("当前档案与切换记录不一致，禁止覆盖身份索引。");
    }
    auto original = load(storage, key), previousData = load(storage, previous), nextData = load(storage, next);
    if (!original || !previousData || !nextData || !canonical(*previousData) || !canonical(*nextData)) {
        return refuse("当前或目标档案无法安全读取。");
    }
    auto index = CompoundTag::fromBinaryNbt(*original);
    if (!index) {
        return refuse("身份索引格式无效。");
    }
    (*index)["ServerId"] = next;
    auto expected        = index->toBinaryNbt();
    auto aliasOriginal   = alias == key ? original : load(storage, alias);
    if (!aliasOriginal) {
        return refuse("国际服身份别名无法读取。");
    }
    auto aliasIndex = CompoundTag::fromBinaryNbt(*aliasOriginal);
    if (!aliasIndex || field(*aliasIndex, "MsaId") != msa
        || (!selfSigned.empty() && field(*aliasIndex, "SelfSignedId") != selfSigned)
        || field(*aliasIndex, "ServerId") != previous) {
        return refuse("国际服身份别名与当前档案不一致。");
    }
    (*aliasIndex)["ServerId"] = next;
    auto aliasExpected        = aliasIndex->toBinaryNbt();
    auto backup               = dir / "switches" / id;
    auto manifestPath         = backup / "manifest.json";
    if (std::filesystem::exists(manifestPath)) {
        auto text = readFile(manifestPath), saved = readFile(backup / "identity.bin"),
             savedData = readFile(backup / "previous.bin"), savedAlias = readFile(backup / "identity-alias.bin");
        auto manifest = text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json{};
        if (!manifest.is_object() || manifest.value("xuid", "") != xuid
            || manifest.value("world", "") != a.at("world").get<std::string>()
            || manifest.value("previous_key", "") != previous || manifest.value("next_key", "") != next || !saved
            || !savedData || sha256(*saved) != manifest.value("identity_hash", "")
            || sha256(*savedData) != manifest.value("previous_hash", "") || *saved != *original
            || *savedData != *previousData || !savedAlias || *savedAlias != *aliasOriginal) {
            return refuse("切换备份与当前档案不一致，请联系管理员。");
        }
    } else {
        nlohmann::json manifest   = pending;
        manifest["xuid"]          = xuid;
        manifest["world"]         = a.at("world");
        manifest["identity_hash"] = sha256(*original);
        manifest["previous_hash"] = sha256(*previousData);
        if (!durableWrite(backup / "identity.bin", *original) || !durableWrite(backup / "previous.bin", *previousData)
            || !durableWrite(backup / "identity-alias.bin", *aliasOriginal)
            || !durableWrite(manifestPath, manifest.dump(2))) {
            return refuse("切换备份失败，未修改身份索引。");
        }
    }
    LevelStorageWriteBatch batch;
    batch.putKey(key, expected, Category);
    if (alias != key) {
        batch.putKey(alias, aliasExpected, Category);
    }
    storage.saveData(batch);
    storage.flushToPermanentStorage();
    auto actual = load(storage, key), old = load(storage, previous), selected = load(storage, next);
    auto actualAlias = load(storage, alias);
    if (!actual || *actual != expected || !actualAlias || *actualAlias != aliasExpected || !old || *old != *previousData
        || !selected || *selected != *nextData) {
        return refuse("切换回读检查失败，所有档案和备份均保留。");
    }
    return true;
}
} // namespace profiles

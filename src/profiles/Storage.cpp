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

std::optional<std::string> validatedPlayerNbt(std::string_view data) {
    auto clean = sanitizePlayerNbt(data);
    if (!clean) {
        return {};
    }
    auto tag = CompoundTag::fromBinaryNbt(*clean);
    if (!tag || !isPlayer(*tag)) {
        return {};
    }
    return clean;
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
        if (!data || !validatedPlayerNbt(*data)) {
            continue;
        }
        auto hash = sha256(*data);
        if (!hash.empty()) {
            result.push_back({identity, key, hash});
        }
        if (!result.empty()) {
            break; // Prefer the exported account key, never historical alternatives.
        }
    }
    return result;
}

std::optional<std::string> identityDataKey(LevelStorage& storage, std::string const& key, std::string const& msa) {
    if (key != "player_" + msa || msa.empty()) {
        return {};
    }
    auto bytes = load(storage, key);
    if (!bytes) {
        return {};
    }
    auto tag = CompoundTag::fromBinaryNbt(*bytes);
    if (!tag || field(*tag, "MsaId") != msa) {
        return {};
    }
    auto dataKey = field(*tag, "ServerId");
    if (!dataKey.starts_with("player_") || dataKey == key) {
        return {};
    }
    return dataKey;
}

std::optional<std::string> profileHash(LevelStorage& storage, std::string const& key) {
    auto data = load(storage, key);
    if (!data || !validatedPlayerNbt(*data)) {
        return {};
    }
    auto hash = sha256(*data);
    return hash.empty() ? std::optional<std::string>{} : hash;
}

bool prepareProfile(LevelStorage& storage, nlohmann::json& p, std::function<bool()> const& commit, std::string& error) {
    auto refuse = [&](std::string text) {
        error = std::move(text);
        return false;
    };
    auto key      = p.at("data_key").get<std::string>();
    auto identity = p.at("source_identity_key").get<std::string>();
    if (!safeProfileId(p.at("id").get<std::string>()) || !key.starts_with("player_") || !identity.starts_with("player_")
        || !uuidMatchesUid(identity.substr(7), p.at("uid").get<uint32_t>())) {
        return refuse("档案标识不匹配，未修改存档。");
    }
    auto source = load(storage, key), indexBytes = load(storage, identity);
    if (!source || !indexBytes) {
        return refuse("网易档案或身份索引无法读取。");
    }
    if (identity != key) {
        auto indexData = sanitizePlayerNbt(*indexBytes);
        if (!indexData) {
            return refuse("网易身份索引无法解析。");
        }
        auto index = CompoundTag::fromBinaryNbt(*indexData);
        if (!index || field(*index, "ServerId") != key) {
            return refuse("网易档案索引已变化，未修复档案。");
        }
    }
    auto data = validatedPlayerNbt(*source);
    if (!data) {
        return refuse("网易档案无法安全解析。");
    }
    auto expected      = sha256(*data);
    auto originalHash  = p.at("source_hash").get<std::string>();
    auto savedExpected = p.at("expected_hash").get<std::string>();
    auto actualHash    = sha256(*source);
    if (expected.empty() || actualHash.empty()
        || (actualHash != originalHash && (savedExpected.empty() || actualHash != savedExpected))
        || (!savedExpected.empty() && savedExpected != expected)) {
        return refuse("网易档案与待修复记录不一致，未覆盖档案。");
    }
    // Commit the two digests before modifying the original key. No player NBT is
    // stored outside the world. A retry accepts exactly the old or repaired bytes.
    if (savedExpected.empty()) {
        p["expected_hash"] = expected;
        if (!commit()) {
            p["expected_hash"] = savedExpected;
            return refuse("修复登记保存失败，未修改档案。");
        }
    }
    if (*source != *data) {
        LevelStorageWriteBatch batch;
        batch.putKey(key, *data, Category);
        storage.saveData(batch);
        storage.flushToPermanentStorage();
    }
    auto actual         = load(storage, key);
    auto unchangedIndex = identity == key ? indexBytes : load(storage, identity);
    if (!actual || *actual != *data || !unchangedIndex || *unchangedIndex != *indexBytes) {
        return refuse("档案修复回读检查失败，请联系管理员。");
    }
    return true;
}

bool switchProfile(LevelStorage& storage, nlohmann::json const& a, std::string& error) {
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
    if (!safeProfileId(id) || key != "player_" + msa || msa.empty() || !previous.starts_with("player_")
        || !next.starts_with("player_") || previous == next || previous == key || next == key || previous == alias
        || next == alias) {
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
    if (!original || !previousData || !nextData || !validatedPlayerNbt(*previousData)
        || !validatedPlayerNbt(*nextData)) {
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
    (*aliasIndex)["ServerId"]            = next;
    auto                   aliasExpected = aliasIndex->toBinaryNbt();
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
        return refuse("切换回读检查失败，请联系管理员。");
    }
    return true;
}
} // namespace profiles

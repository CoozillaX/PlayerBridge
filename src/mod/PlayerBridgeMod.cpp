#include "mod/PlayerBridgeMod.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/server/ServerPlayer.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/storage/LevelStorage.h"
#include "mc/world/level/storage/PlayerDataSystem.h"
#include "mc/world/level/storage/PlayerStorageIds.h"
#include "profiles/Files.h"
#include "profiles/Identity.h"
#include <algorithm>
#include <sstream>

namespace player_bridge {
namespace {
using Json = nlohmann::json;

std::string account(Player const& p) {
    return p.getUuid().asString();
}

bool authenticatedServer() {
    auto text = profiles::readFile(std::filesystem::current_path() / "server.properties");
    if (!text) {
        return false;
    }
    std::istringstream input(*text);
    std::string        line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "online-mode=true") {
            return true;
        }
    }
    return false;
}

std::string activeKey(Json const& r, Json const& a, std::string const& xuid) {
    auto active = a.at("active_profile").get<std::string>();
    if (active == "default") {
        return a.at("default_key").get<std::string>();
    }
    auto* p = profiles::ownedProfile(r, active, xuid);
    return p ? p->at("data_key").get<std::string>() : std::string{};
}
} // namespace

PlayerBridgeMod& PlayerBridgeMod::getInstance() {
    static PlayerBridgeMod instance;
    return instance;
}

LL_STATIC_HOOK(
    ProfileBeforeLoad,
    ll::memory::HookPriority::Normal,
    &PlayerDataSystem::loadPlayerData,
    std::unique_ptr<CompoundTag>,
    LevelStorage&           storage,
    PlayerStorageIds const& ids,
    bool                    host,
    bool                    editor
) {
    PlayerBridgeMod::getInstance().beforePlayerLoad(storage, ids.MsaId.get(), ids.SelfSignedId.get());
    return origin(storage, ids, host, editor);
}

LL_TYPE_INSTANCE_HOOK(
    ProfileAfterLeave,
    ll::memory::HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::_onPlayerLeft,
    void,
    ServerPlayer* player,
    bool          skipMessage
) {
    auto xuid = player ? player->getXuid() : std::string{};
    origin(player, skipMessage);
    PlayerBridgeMod::getInstance().afterPlayerLeft(xuid);
}

bool PlayerBridgeMod::load() {
    try {
        if (!authenticatedServer()) {
            getSelf().getLogger().error("Profile ownership requires online-mode=true");
            return false;
        }
        mDataDir  = getSelf().getDataDir();
        auto path = getSelf().getConfigDir() / "config.json";
        Json defaults{
            {"api", {{"base_url", "https://api.liliya233.uk"}, {"api_key", ""}, {"timeout_seconds", 15}}},
            {"challenge_ttl_seconds", 600},
            {"verify_cooldown_seconds", 10}
        };
        auto text = profiles::readFile(path);
        if (!text) {
            if (!profiles::durableWrite(path, defaults.dump(2))) {
                return false;
            }
            text = defaults.dump();
        }
        auto config = Json::parse(*text, nullptr, false);
        if (!config.is_object()) {
            return false;
        }
        auto const& api = config.at("api");
        mApi            = {
            api.at("base_url").get<std::string>(),
            api.at("api_key").get<std::string>(),
            std::clamp(api.value("timeout_seconds", 15), 1, 60)
        };
        mTtl      = std::clamp(config.value("challenge_ttl_seconds", 600), 60, 1800);
        mCooldown = std::clamp(config.value("verify_cooldown_seconds", 10), 5, 300);
        std::string error;
        if (!mStore.open(mDataDir / "profiles.leveldb", error) || !mStore.load(mRegistry, error)) {
            getSelf().getLogger().error("Profile database open failed: {}", error);
            return false;
        }
        return true;
    } catch (...) {
        getSelf().getLogger().error("Invalid profile registry or configuration (credentials omitted)");
        return false;
    }
}

bool PlayerBridgeMod::saveState() {
    if (!profiles::validateRegistry(mRegistry)) {
        mStateHealthy = false;
        getSelf().getLogger().error("Profile registry validation failed; further changes disabled");
        return false;
    }
    std::string error;
    if (mStore.save(mRegistry, error)) {
        return true;
    }
    mStateHealthy = false;
    getSelf().getLogger().error("Profile database transaction failed; further changes disabled: {}", error);
    return false;
}

bool PlayerBridgeMod::enable() {
    static ll::memory::HookRegistrar<ProfileBeforeLoad> profileHook;
    static ll::memory::HookRegistrar<ProfileAfterLeave> leaveHook;
    mEnabled = true;
    for (auto const& [xuid, a] : mRegistry["accounts"].items()) {
        if (!a["pending"].is_null() && a["pending"]["status"] != "failed") {
            mOfflineJobs.insert(xuid);
        }
    }
    for (auto const& p : mRegistry["profiles"]) {
        if (p["status"] == "adding") {
            mOfflineJobs.insert(p["owner"].get<std::string>());
        }
    }
    auto& command = ll::command::CommandRegistrar::getInstance(false)
                        .getOrCreateCommand("profile", "找回和切换网易角色", CommandPermissionLevel::Any);
    command.overload().execute([this](CommandOrigin const& origin, CommandOutput& output) {
        if (!mEnabled) {
            output.error("档案插件已停用");
            return;
        }
        auto* entity = origin.getEntity();
        if (!entity || !entity->isPlayer()) {
            output.error("请在游戏内使用 /profile");
            return;
        }
        openMenu(*static_cast<Player*>(entity));
    });
    auto& bus = ll::event::EventBus::getInstance();
    // Pending switches are applied before loadPlayerData, never in join events.
    mListeners.push_back(bus.emplaceListener<ll::event::PlayerDisconnectEvent>([this](auto& e) {
        auto key = account(e.self());
        mFormTokens.erase(key);
        mRequests.erase(key);
        mLastAttempt.erase(key);
        auto login = mStorageLogins.find(key);
        if (login != mStorageLogins.end()) {
            auto ids = login->second;
            mStorageLogins.erase(ids.msa);
            mStorageLogins.erase(ids.selfSigned);
        }
    }));
    mListeners.push_back(bus.emplaceListener<ll::event::PlayerJoinEvent>([this](auto& e) {
        auto xuid   = e.self().getXuid();
        auto reason = mBlocked.find(xuid);
        if (reason == mBlocked.end()) {
            reason = mBlocked.find(account(e.self()));
        }
        bool pending = mRegistry["accounts"].contains(xuid) && !mRegistry["accounts"][xuid]["pending"].is_null();
        if (reason != mBlocked.end() || pending || !mStateHealthy) {
            e.cancel();
            static_cast<Player&>(e.self()).disconnect(
                reason != mBlocked.end() ? reason->second : "档案切换尚未完成，请稍后重新加入或联系管理员。"
            );
        }
    }));
    // No startup enumeration of player records or world data.
    return std::all_of(mListeners.begin(), mListeners.end(), [](auto const& p) {
        return bool(p);
    });
}

bool PlayerBridgeMod::disable() {
    mEnabled = false;
    mWorkers.clear();
    for (auto const& p : mListeners) {
        ll::event::EventBus::getInstance().removeListener(p);
    }
    mListeners.clear();
    mFormTokens.clear();
    mRequests.clear();
    mLastAttempt.clear();
    mBlocked.clear();
    mStorageLogins.clear();
    mOfflineJobs.clear();
    return true;
}

bool PlayerBridgeMod::beforePlayerLoad(LevelStorage& storage, std::string const& msa, std::string const& selfSigned) {
    if (!mEnabled) {
        return true;
    }
    mStorageLogins[msa] = {msa, selfSigned};
    if (!selfSigned.empty()) {
        mStorageLogins[selfSigned] = {msa, selfSigned};
    }
    std::string xuid;
    for (auto const& [owner, a] : mRegistry["accounts"].items()) {
        if (a.at("msa_id").get<std::string>() == msa) {
            xuid = owner;
            break;
        }
    }
    auto world = storage.getFullPath().value;
    for (auto const& p : mRegistry["profiles"]) {
        if (p["world"].get<std::string>() == world
            && (p["source_identity_key"].get<std::string>() == "player_" + msa
                || p["source_identity_key"].get<std::string>() == "player_" + selfSigned)
            && p["owner"].get<std::string>() != xuid) {
            mBlocked[selfSigned] = "该原始档案已归属其他账号。";
            mBlocked[msa]        = "该原始档案已归属其他账号。";
            return false;
        }
    }
    if (xuid.empty()) {
        return true;
    }
    mBlocked.erase(xuid);
    if (!mStateHealthy) {
        mBlocked[xuid] = "档案登记文件无法写入，请联系管理员。";
        return false;
    }
    if (!mRegistry["accounts"].contains(xuid)) {
        return true;
    }
    auto& a = mRegistry["accounts"][xuid];
    if (a["world"].get<std::string>() != world
        || (!selfSigned.empty() && a.at("self_signed_id").get<std::string>() != selfSigned)) {
        mBlocked[xuid] = "账号身份或世界与档案登记不一致，请联系管理员。";
        return false;
    }
    if (mOfflineJobs.contains(xuid)) {
        auto level = ll::service::getLevel();
        if (level) {
            try {
                processOfflineJobs(*level, xuid);
            } catch (...) {
                mStateHealthy  = false;
                mBlocked[xuid] = "档案应用异常，所有档案均保留，请联系管理员。";
                return false;
            }
        }
    }
    if (!a["pending"].is_null()) {
        mBlocked[xuid] = "离线档案切换尚未完成，请稍后重新加入或联系管理员。";
        return false;
    }
    for (auto const& p : mRegistry["profiles"]) {
        if (p["owner"].get<std::string>() == xuid && p["status"] == "adding") {
            mBlocked[xuid] = "档案恢复尚未完成，请稍后重新加入。";
            return false;
        }
    }
    auto key = profiles::identityDataKey(storage, a["identity_key"].get<std::string>(), msa);
    if (!key || *key != activeKey(mRegistry, a, xuid)) {
        mBlocked[xuid] = "当前档案索引与登记不一致，请联系管理员。";
        return false;
    }
    return true;
}

void PlayerBridgeMod::afterPlayerLeft(std::string const& xuid) {
    if (mEnabled && mRegistry["accounts"].contains(xuid) && !mRegistry["accounts"][xuid]["pending"].is_null()) {
        mOfflineJobs.insert(xuid);
    }
}

void PlayerBridgeMod::processOfflineJobs(Level& level, std::string const& requestedOwner) {
    if (!mEnabled || !mStateHealthy || mOfflineJobs.empty()) {
        return;
    }
    std::string xuid;
    for (auto const& owner : mOfflineJobs) {
        if (owner != requestedOwner) {
            continue;
        }
        bool online = false;
        level.forEachPlayer([&](Player& p) {
            if (p.getXuid() == owner) {
                online = true;
            }
            return true;
        });
        if (!online) {
            xuid = owner;
            break;
        }
    }
    if (xuid.empty()) {
        return;
    }
    mOfflineJobs.erase(xuid);
    if (!mRegistry["accounts"].contains(xuid)) {
        return;
    }
    auto& a       = mRegistry["accounts"][xuid];
    auto& storage = level.getLevelStorage();
    if (a["world"].get<std::string>() != storage.getFullPath().value) {
        return;
    }
    // The network leave handler has returned and the old player is absent.
    // Flush its completed saves before changing any identity index.
    storage.flushToPermanentStorage();
    for (auto& p : mRegistry["profiles"]) {
        if (p["owner"].get<std::string>() != xuid || p["status"].get<std::string>() != "adding") {
            continue;
        }
        std::string error;
        bool        ok = false;
        try {
            ok = profiles::prepareProfile(
                storage,
                p,
                [this] {
                    return saveState();
                },
                error
            );
        } catch (...) {
            error = "档案修复异常，请联系管理员。";
        }
        p["status"] = ok ? "ready" : "failed";
        if (ok) {
            p.erase("source_hash");
            p.erase("expected_hash");
            p.erase("error");
        } else {
            p["error"] = error;
        }
        if (!saveState()) {
            mBlocked[xuid] = "档案登记保存失败，请联系管理员。";
            return;
        }
    }
    auto& pending = a["pending"];
    if (!pending.is_null()) {
        if (pending["status"].get<std::string>() == "failed") {
            mBlocked[xuid] = "上次档案切换失败，所有档案均保留，请联系管理员。";
            return;
        }
        pending["status"] = "applying";
        if (!saveState()) {
            mBlocked[xuid] = "切换记录保存失败，请联系管理员。";
            return;
        }
        std::string error;
        bool        ok = false;
        try {
            ok = profiles::switchProfile(storage, a, error);
        } catch (...) {
            error = "档案切换异常，请联系管理员。";
        }
        if (ok) {
            a["active_profile"] = pending["profile_id"];
            a["pending"]        = nullptr;
            mBlocked.erase(xuid);
        } else {
            pending["status"] = "failed";
            pending["error"]  = error;
            mBlocked[xuid]    = error;
        }
        if (!saveState()) {
            mBlocked[xuid] = "切换结果保存失败，请联系管理员。";
        } else if (ok) {
            getSelf().getLogger().info("Pre-load profile switch completed; journal saved");
        }
    }
}

bool PlayerBridgeMod::ensureAccount(Player& player) {
    auto xuid = player.getXuid();
    if (!mStateHealthy) {
        showNotice(player, "档案登记校验或保存失败，插件已暂停操作。请联系管理员检查服务端日志。");
        return false;
    }
    if (xuid.empty()) {
        showNotice(player, "未取得国际服账号的 XUID，请使用已登录的账号加入。");
        return false;
    }
    auto login = mStorageLogins.find(account(player));
    if (login == mStorageLogins.end() || login->second.msa.empty()) {
        showNotice(player, "尚未捕获引擎存档身份，请重新加入后再试。");
        return false;
    }
    auto level = ll::service::getLevel();
    if (!level) {
        return false;
    }
    auto& storage = level->getLevelStorage();
    auto  world   = storage.getFullPath().value;
    if (mRegistry["accounts"].contains(xuid)) {
        auto const& a = mRegistry["accounts"][xuid];
        if (a["world"].get<std::string>() != world || a["uuid"].get<std::string>() != account(player)
            || a.at("msa_id").get<std::string>() != login->second.msa || !a["pending"].is_null()) {
            showNotice(player, "身份或世界不一致，或仍有待完成的切换，请重新加入或联系管理员。");
            return false;
        }
        auto key =
            profiles::identityDataKey(storage, a["identity_key"].get<std::string>(), a.at("msa_id").get<std::string>());
        if (!key || *key != activeKey(mRegistry, a, xuid)) {
            showNotice(player, "档案索引与登记不一致，请联系管理员。");
            return false;
        }
        return true;
    }
    // Ask the engine to persist the original international profile before registering its slot.
    storage.save(player);
    storage.flushToPermanentStorage();
    auto identity = PlayerDataSystem::playerKey(login->second.msa);
    auto key      = profiles::identityDataKey(storage, identity, login->second.msa);
    if (!key || key->starts_with("player_server_profiles_") || !profiles::profileHash(storage, *key)) {
        showNotice(player, "初始档案尚未保存，请退出重进后再打开档案列表。");
        return false;
    }
    // Authenticated BDS logins may omit SelfSignedId in the incoming storage IDs.
    // Recover an existing alias only when its persisted MSA identity matches this login.
    if (login->second.selfSigned.empty()) {
        auto index = storage.getCompoundTag(identity, DBHelpers::Category::Player);
        if (index && index->contains("SelfSignedId", Tag::String)) {
            auto self = std::string((*index)["SelfSignedId"].get<StringTag>());
            if (!self.empty()) {
                auto alias = storage.getCompoundTag(PlayerDataSystem::playerKey(self), DBHelpers::Category::Player);
                if (alias && alias->contains("MsaId", Tag::String) && alias->contains("ServerId", Tag::String)
                    && std::string((*alias)["MsaId"].get<StringTag>()) == login->second.msa
                    && std::string((*alias)["ServerId"].get<StringTag>()) == *key) {
                    login->second.selfSigned = self;
                }
            }
        }
    }
    for (auto const& p : mRegistry["profiles"]) {
        if (p["world"].get<std::string>() == world && p["data_key"].get<std::string>() == *key) {
            showNotice(player, "当前档案已有归属记录，请联系管理员。");
            return false;
        }
    }
    mRegistry["accounts"][xuid] = Json{
        {"world", world},
        {"uuid", account(player)},
        {"identity_key", identity},
        {"msa_id", login->second.msa},
        {"self_signed_id", login->second.selfSigned},
        {"default_key", *key},
        {"active_profile", "default"},
        {"pending", nullptr}
    };
    if (!saveState()) {
        showNotice(player, "初始档案登记失败，存档未删除。");
        return false;
    }
    return true;
}

void PlayerBridgeMod::showNotice(Player& p, std::string const& text) {
    ll::form::SimpleForm form("网易角色", text);
    form.appendButton("返回档案菜单", [this](Player& p) {
        if (mEnabled && mStateHealthy) {
            openMenu(p);
        }
    });
    form.sendTo(p);
}

void PlayerBridgeMod::openMenu(Player& p) {
    if (!ensureAccount(p)) {
        return;
    }
    // Reopening the menu invalidates any old verification form/request for this session.
    auto key = account(p);
    mFormTokens.erase(key);
    mRequests.erase(key);
    ll::form::SimpleForm form(
        "网易角色",
        "初始档案和已找回的网易档案均会保留。切换时自动退出，重新加入后加载所选档案。"
    );
    form.appendButton("我的档案 / 切换档案", [this](Player& p) {
        openProfiles(p);
    });
    form.appendButton("找回网易角色", [this](Player& p) {
        openUidForm(p);
    });
    form.sendTo(p);
}

void PlayerBridgeMod::openUidForm(Player& p) {
    if (!ensureAccount(p)) {
        return;
    }
    if (mApi.apiKey.empty()) {
        showNotice(p, "管理员尚未配置验证服务。");
        return;
    }
    auto key = account(p);
    mFormTokens.erase(key);
    mRequests.erase(key);
    ll::form::CustomForm form("找回网易角色");
    form.appendInput("uid", "网易 UID", "填写数字");
    form.setSubmitButton("下一步");
    form.sendTo(p, [this](Player& p, ll::form::CustomFormResult const& result, auto) {
        if (!mEnabled || !result) {
            return;
        }
        auto it = result->find("uid");
        if (it == result->end() || !std::holds_alternative<std::string>(it->second)) {
            return;
        }
        auto uid = profiles::parseUid(std::get<std::string>(it->second));
        if (!uid) {
            showNotice(p, "UID 必须是 1 到 4294967295 的整数。");
            return;
        }
        auto world = mRegistry["accounts"][p.getXuid()]["world"].get<std::string>();
        if (!profiles::mayClaim(mRegistry, world, *uid, "", p.getXuid())) {
            showNotice(p, "该网易账号的档案已归属另一国际服账号。");
            return;
        }
        openVerification(p, *uid);
    });
}

void PlayerBridgeMod::openVerification(Player& p, uint32_t uid) {
    auto code = profiles::makeVerificationCode(), token = profiles::makeChallenge();
    if (code.empty() || token.empty()) {
        showNotice(p, "验证码生成失败，请重试。");
        return;
    }
    auto key = account(p), xuid = p.getXuid();
    mFormTokens[key]             = token;
    auto                 expires = std::chrono::steady_clock::now() + std::chrono::seconds(mTtl);
    ll::form::SimpleForm form(
        "验证网易账号",
        "网易 UID：" + std::to_string(uid) + "\n\n请把网易签名修改为：\n\n" + code
            + "\n\n修改后点击“完成”。关闭本表单会作废验证码。"
    );
    form.appendButton("完成");
    form.sendTo(p, [this, key, xuid, uid, code, token, expires](Player& p, int selected, auto) {
        auto it = mFormTokens.find(key);
        if (it == mFormTokens.end() || it->second != token) {
            return;
        }
        mFormTokens.erase(it); // Close, cancel and submit all consume this form's code.
        if (mEnabled && selected == 0 && account(p) == key && p.getXuid() == xuid) {
            verify(p, uid, code, token, expires);
        }
    });
}

void PlayerBridgeMod::verify(
    Player&                               p,
    uint32_t                              uid,
    std::string                           code,
    std::string                           token,
    std::chrono::steady_clock::time_point expires
) {
    auto now     = std::chrono::steady_clock::now();
    auto keyTime = mLastAttempt.find(account(p));
    if (expires <= now) {
        showNotice(p, "验证码已过期，请重新验证。");
        return;
    }
    if (keyTime != mLastAttempt.end() && now - keyTime->second < std::chrono::seconds(mCooldown)) {
        showNotice(p, "验证过于频繁，请稍后重新验证。");
        return;
    }
    std::erase_if(mWorkers, [](Worker const& w) {
        return w.done->load();
    });
    if (mWorkers.size() >= 8) {
        showNotice(p, "验证服务繁忙，请稍后重新验证。");
        return;
    }
    auto key = account(p), xuid = p.getXuid();
    mLastAttempt[key] = now;
    mRequests[key]    = token;
    // The code form has just closed. Show only the result, avoiding overlapping modal forms while HTTP is running.
    auto done = std::make_shared<std::atomic_bool>(false);
    mWorkers.push_back({done, std::jthread([this, key, xuid, uid, code, token, expires, config = mApi, done] {
                            auto result = profiles::fetchProfile(config, uid);
                            ll::thread::ServerThreadExecutor::getDefault().execute(
                                [this, key, xuid, uid, code, token, expires, result = std::move(result)] {
                                    auto it = mRequests.find(key);
                                    if (!mEnabled || it == mRequests.end() || it->second != token) {
                                        return;
                                    }
                                    mRequests.erase(it);
                                    auto  level = ll::service::getLevel();
                                    auto* p     = level ? level->getPlayer(mce::UUID::fromString(key)) : nullptr;
                                    if (!p || p->getXuid() != xuid) {
                                        return;
                                    }
                                    if (expires <= std::chrono::steady_clock::now()) {
                                        showNotice(*p, "验证码已过期，请重新验证。");
                                        return;
                                    }
                                    if (!result.success) {
                                        showNotice(*p, result.error + "\n请重新验证。");
                                        return;
                                    }
                                    if (profiles::trimSignature(result.signature) != code) {
                                        showNotice(*p, "签名不匹配，请重新生成验证码后再验证。");
                                        return;
                                    }
                                    showVerifiedSources(*p, uid, result.nickname);
                                }
                            );
                            done->store(true);
                        })});
}

void PlayerBridgeMod::showVerifiedSources(Player& p, uint32_t uid, std::string const& nickname) {
    if (!ensureAccount(p)) {
        return;
    }
    auto level = ll::service::getLevel();
    if (!level) {
        return;
    }
    auto world = mRegistry["accounts"][p.getXuid()]["world"].get<std::string>();
    if (!profiles::mayClaim(mRegistry, world, uid, "", p.getXuid())) {
        showNotice(p, "该网易账号的档案已归属另一国际服账号。");
        return;
    }
    auto sources = profiles::findSources(level->getLevelStorage(), uid);
    std::erase_if(sources, [&](auto const& source) {
        return std::any_of(
            mRegistry["profiles"].begin(),
            mRegistry["profiles"].end(),
            [&](nlohmann::json const& profile) {
                return profile["world"].get<std::string>() == world
                    && profile["data_key"].get<std::string>() == source.dataKey;
            }
        );
    });
    if (sources.empty()) {
        showNotice(p, "验证成功，但当前世界中没有可找回的新档案。已找回的档案可在“我的档案”中查看。");
        return;
    }
    if (sources.size() == 1) {
        addProfile(p, uid, nickname, sources.front());
        return;
    }
    auto key = account(p), xuid = p.getXuid(), token = profiles::makeChallenge();
    if (token.empty()) {
        return;
    }
    mFormTokens[key]             = token;
    auto                 expires = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    ll::form::SimpleForm form(
        "选择要找回的档案",
        "验证成功。该网易 UID 有多份角色档案，选择一份加入列表。原始档案不会删除。"
    );
    for (auto const& source : sources) {
        form.appendButton(nickname + "\n" + source.identityKey.substr(7));
    }
    form.sendTo(p, [this, key, xuid, token, uid, nickname, sources, expires](Player& p, int selected, auto) {
        auto it = mFormTokens.find(key);
        if (it == mFormTokens.end() || it->second != token) {
            return;
        }
        mFormTokens.erase(it);
        if (!mEnabled || p.getXuid() != xuid || selected < 0 || static_cast<size_t>(selected) >= sources.size()
            || expires < std::chrono::steady_clock::now()) {
            return;
        }
        addProfile(p, uid, nickname, sources[static_cast<size_t>(selected)]);
    });
}

void PlayerBridgeMod::addProfile(
    Player&                       p,
    uint32_t                      uid,
    std::string const&            nickname,
    profiles::SourceRecord const& source
) {
    if (!ensureAccount(p)) {
        return;
    }
    auto xuid = p.getXuid(), world = mRegistry["accounts"][xuid]["world"].get<std::string>();
    if (!profiles::mayClaim(mRegistry, world, uid, source.dataKey, xuid)) {
        showNotice(p, "该档案已归属另一国际服账号。");
        return;
    }
    for (auto const& entry : mRegistry["profiles"]) {
        if (entry["world"].get<std::string>() == world && entry["data_key"].get<std::string>() == source.dataKey) {
            showNotice(p, "该档案已有登记，请在列表中查看。");
            return;
        }
    }
    for (auto const& [other, a] : mRegistry["accounts"].items()) {
        if (a["world"].get<std::string>() == world && a["default_key"].get<std::string>() == source.dataKey) {
            showNotice(p, "该档案已登记为账号的初始档案。");
            return;
        }
    }
    auto level = ll::service::getLevel();
    if (!level) {
        return;
    }
    bool occupied = false;
    level->forEachPlayer([&](Player& other) {
        auto login = mStorageLogins.find(account(other));
        if (login == mStorageLogins.end()) {
            return true;
        }
        auto const& ids     = login->second;
        auto        dataKey = profiles::identityDataKey(level->getLevelStorage(), "player_" + ids.msa, ids.msa);
        if ("player_" + ids.msa == source.identityKey || "player_" + ids.selfSigned == source.identityKey
            || (dataKey && *dataKey == source.dataKey)) {
            occupied = true;
        }
        return true;
    });
    if (occupied) {
        showNotice(p, "该网易档案正在被使用，请等对应玩家退出后再验证。");
        return;
    }
    auto id = profiles::makeChallenge();
    if (id.empty()) {
        return;
    }
    Json profile{
        {"id", id},
        {"owner", xuid},
        {"uid", uid},
        {"world", world},
        {"name", nickname + "（UID " + std::to_string(uid) + "）"},
        {"source_identity_key", source.identityKey},
        {"source_hash", source.hash},
        {"expected_hash", ""},
        {"data_key", source.dataKey},
        {"status", "adding"}
    };
    // Reserve ownership before repairing the original world record in place.
    mRegistry["profiles"].push_back(std::move(profile));
    if (!saveState()) {
        showNotice(p, "档案归属登记失败，原始档案未修改。");
        return;
    }
    auto&       entry = mRegistry["profiles"].back();
    std::string error;
    bool        ok = false;
    try {
        ok = profiles::prepareProfile(
            level->getLevelStorage(),
            entry,
            [this] {
                return saveState();
            },
            error
        );
    } catch (...) {
        error = "档案修复异常，请联系管理员。";
    }
    entry["status"] = ok ? "ready" : "failed";
    if (ok) {
        entry.erase("source_hash");
        entry.erase("expected_hash");
        entry.erase("error");
    } else {
        entry["error"] = error;
    }
    if (!saveState()) {
        showNotice(p, "档案登记保存失败，请联系管理员。");
        return;
    }
    if (!ok) {
        showNotice(p, error);
        return;
    }
    ll::form::SimpleForm form("角色已找回", "已修复原网易档案，可在“我的档案”中选择使用。初始档案保留。");
    form.appendButton("查看档案列表", [this](Player& p) {
        if (mEnabled) {
            openProfiles(p);
        }
    });
    form.sendTo(p);
}

void PlayerBridgeMod::openProfiles(Player& p) {
    if (!ensureAccount(p)) {
        return;
    }
    auto                 xuid   = p.getXuid();
    auto const&          a      = mRegistry["accounts"][xuid];
    auto                 active = a["active_profile"].get<std::string>();
    ll::form::SimpleForm form("我的档案", "选择档案后可切换。每份档案分别保存背包、经验、位置等进度。");
    form.appendButton(std::string(active == "default" ? "【使用中】" : "") + "初始档案", [this](Player& p) {
        if (mEnabled) {
            chooseProfile(p, "default");
        }
    });
    for (auto const& entry : mRegistry["profiles"]) {
        if (entry["owner"].get<std::string>() == xuid) {
            auto id = entry["id"].get<std::string>(), name = entry["name"].get<std::string>();
            auto status = entry["status"].get<std::string>();
            form.appendButton(
                (active == id        ? "【使用中】"
                 : status == "ready" ? ""
                                     : "【需管理员检查】")
                    + name,
                [this, id](Player& p) {
                    if (mEnabled) {
                        chooseProfile(p, id);
                    }
                }
            );
        }
    }
    form.appendButton("返回", [this](Player& p) {
        if (mEnabled) {
            openMenu(p);
        }
    });
    form.sendTo(p);
}

void PlayerBridgeMod::chooseProfile(Player& p, std::string const& id) {
    if (!ensureAccount(p)) {
        return;
    }
    auto        xuid = p.getXuid();
    auto const& a    = mRegistry["accounts"][xuid];
    if (a["active_profile"].get<std::string>() == id) {
        showNotice(p, "正在使用这份档案。");
        return;
    }
    std::string name = "初始档案";
    if (id != "default") {
        auto* entry = profiles::ownedProfile(mRegistry, id, xuid);
        if (!entry || entry->at("status") != "ready") {
            showNotice(p, "档案不可用，请联系管理员。原始记录已保留。");
            return;
        }
        name = entry->at("name").get<std::string>();
    }
    auto                 expires = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    ll::form::SimpleForm form(
        "切换档案",
        "切换至：" + name + "\n\n当前进度会正常保存，随后断开连接。请重新加入服务器，使用所选档案。所有档案均保留。"
    );
    form.appendButton("确认切换", [this, id, xuid, expires](Player& p) {
        if (mEnabled && p.getXuid() == xuid && std::chrono::steady_clock::now() < expires) {
            queueSwitch(p, id);
        }
    });
    form.appendButton("返回档案列表", [this](Player& p) {
        if (mEnabled) {
            openProfiles(p);
        }
    });
    form.sendTo(p);
}

void PlayerBridgeMod::queueSwitch(Player& p, std::string const& id) {
    if (!ensureAccount(p)) {
        return;
    }
    auto  xuid = p.getXuid();
    auto& a    = mRegistry["accounts"][xuid];
    if (a["active_profile"].get<std::string>() == id) {
        return;
    }
    auto* entry = id == "default" ? nullptr : profiles::ownedProfile(mRegistry, id, xuid);
    if (id != "default" && (!entry || entry->at("status") != "ready" || entry->at("world") != a["world"])) {
        showNotice(p, "目标档案不属于当前账号或不可用。");
        return;
    }
    auto token = profiles::makeChallenge();
    if (token.empty()) {
        return;
    }
    a["pending"] = Json{
        {"id", token},
        {"profile_id", id},
        {"previous_key", activeKey(mRegistry, a, xuid)},
        {"next_key", entry ? entry->at("data_key") : a["default_key"]},
        {"status", "pending"}
    };
    if (!saveState()) {
        showNotice(p, "切换任务保存失败，所有档案均保留。");
        return;
    }
    // Leave completion marks the old session saved; the next data load applies
    // the pending switch before reading the selected profile.
    p.disconnect("档案切换已确认。请重新加入服务器，使用所选档案。");
}
} // namespace player_bridge

LL_REGISTER_MOD(player_bridge::PlayerBridgeMod, player_bridge::PlayerBridgeMod::getInstance());

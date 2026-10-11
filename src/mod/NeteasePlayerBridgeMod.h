#pragma once
#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"
#include "profiles/ApiClient.h"
#include "profiles/ProfileRegistry.h"
#include "profiles/RegistryStore.h"
#include "profiles/Storage.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <unordered_set>
class Player;
class LevelStorage;
class Level;

namespace netease_player_bridge {
class NeteasePlayerBridgeMod {
public:
    static NeteasePlayerBridgeMod& getInstance();

    NeteasePlayerBridgeMod() : mSelf(*ll::mod::NativeMod::current()) {}

    ll::mod::NativeMod& getSelf() const {
        return mSelf;
    }

    bool load();
    bool enable();
    bool disable();

    // Hooks and queued form callbacks remain resident; dynamic DLL unloading is unsupported.
    bool unload() {
        return false;
    }

    bool beforePlayerLoad(LevelStorage&, std::string const& msa, std::string const& selfSigned);
    void afterPlayerLeft(std::string const& xuid);

private:
    ll::mod::NativeMod&     mSelf;
    profiles::ApiConfig     mApi;
    int                     mTtl = 600, mCooldown = 10;
    bool                    mEnabled = false, mStateHealthy = true;
    profiles::RegistryStore mStore;
    profiles::Registry      mRegistry = profiles::emptyRegistry();
    // These tokens only invalidate in-flight forms/requests. Codes live in form callback captures.
    std::unordered_map<std::string, std::string> mFormTokens, mRequests, mBlocked;

    struct StorageLogin {
        std::string msa, selfSigned;
    };

    std::unordered_map<std::string, StorageLogin> mStorageLogins;
    std::unordered_set<std::string>               mOfflineJobs;
    void                                          processOfflineJobs(Level&, std::string const& owner);
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> mLastAttempt;

    struct Worker {
        std::shared_ptr<std::atomic_bool> done;
        std::jthread                      thread;
    };

    std::vector<Worker>                 mWorkers;
    std::vector<ll::event::ListenerPtr> mListeners;
    std::filesystem::path               mDataDir;
    bool                                saveState();
    bool                                ensureAccount(Player&);
    void                                openMenu(Player&);
    void                                showNotice(Player&, std::string const&);
    void                                openUidForm(Player&);
    void                                openVerification(Player&, uint32_t uid);
    void
    verify(Player&, uint32_t uid, std::string code, std::string token, std::chrono::steady_clock::time_point expires);
    void showVerifiedSources(Player&, uint32_t uid, std::string const& nickname);
    void addProfile(Player&, uint32_t uid, std::string const& nickname, profiles::SourceRecord const&);
    void openProfiles(Player&);
    void chooseProfile(Player&, std::string const& profileId);
    void queueSwitch(Player&, std::string const& profileId);
};
} // namespace netease_player_bridge

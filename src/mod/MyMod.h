#pragma once
#include "ll/api/mod/NativeMod.h"
#include "ll/api/event/ListenerBase.h"
#include "migration/ApiClient.h"
#include "migration/ProfileRegistry.h"
#include "migration/RegistryStore.h"
#include "migration/Storage.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <unordered_set>
class Player;
class LevelStorage;
class Level;
namespace my_mod {
class MyMod {
public:
    static MyMod& getInstance();
    MyMod():mSelf(*ll::mod::NativeMod::current()){}
    ll::mod::NativeMod& getSelf() const {return mSelf;}
    bool load();
    bool enable();
    bool disable();
    bool unload(){return false;}
    bool beforePlayerLoad(LevelStorage&,std::string const& msa,std::string const& selfSigned);
    void afterPlayerLeft(std::string const& xuid);
private:
    ll::mod::NativeMod& mSelf;
    migration::ApiConfig mApi;
    int mTtl=600,mCooldown=10;
    bool mEnabled=false,mStateHealthy=true;
    migration::RegistryStore mStore;
    migration::Registry mRegistry=migration::emptyRegistry();
    // These tokens only invalidate in-flight forms/requests. Codes live in form callback captures.
    std::unordered_map<std::string,std::string> mFormTokens,mRequests,mBlocked;
    struct StorageLogin {std::string msa,selfSigned;};
    std::unordered_map<std::string,StorageLogin> mStorageLogins;
    std::unordered_set<std::string> mOfflineJobs;
    void processOfflineJobs(Level&,std::string const& owner);
    std::unordered_map<std::string,std::chrono::steady_clock::time_point> mLastAttempt;
    struct Worker {std::shared_ptr<std::atomic_bool> done;std::jthread thread;};
    std::vector<Worker> mWorkers;
    std::vector<ll::event::ListenerPtr> mListeners;
    std::filesystem::path mDataDir;
    bool saveState();
    bool ensureAccount(Player&);
    void openMenu(Player&);
    void showNotice(Player&,std::string const&);
    void openUidForm(Player&);
    void openVerification(Player&,uint32_t uid);
    void verify(Player&,uint32_t uid,std::string code,std::string token,std::chrono::steady_clock::time_point expires);
    void showVerifiedSources(Player&,uint32_t uid,std::string const& nickname);
    void addProfile(Player&,uint32_t uid,std::string const& nickname,migration::SourceRecord const&);
    void openProfiles(Player&);
    void chooseProfile(Player&,std::string const& profileId);
    void queueSwitch(Player&,std::string const& profileId);
};
}

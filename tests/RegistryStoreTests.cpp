#include "migration/RegistryStore.h"
#include "migration/Files.h"
#include "migration/Identity.h"
#include <sqlite3.h>
#include <iostream>
#include <stdexcept>
#include <filesystem>

using namespace migration;
void require(bool ok,std::string const& message){if(!ok)throw std::runtime_error(message);}
void sql(std::filesystem::path const& path,char const* text,bool success=true){
    sqlite3* db=nullptr;require(sqlite3_open(path.string().c_str(),&db)==SQLITE_OK,"open test database");
    sqlite3_exec(db,"PRAGMA foreign_keys=ON",nullptr,nullptr,nullptr);
    int result=sqlite3_exec(db,text,nullptr,nullptr,nullptr);std::string error=sqlite3_errmsg(db);sqlite3_close(db);
    require(success?result==SQLITE_OK:result==SQLITE_CONSTRAINT,"SQL constraint/transaction test: "+error);
}
Registry account(std::string id){return {{"world","fixture"},{"uuid","uuid-"+id},{"msa_id",id},{"self_signed_id",""},
    {"identity_key","player_"+id},{"default_key","player_server_original-"+id},{"active_profile","default"},{"pending",nullptr}};}
Registry sample(){
    auto r=emptyRegistry();r["accounts"]["111"]=account("111");r["accounts"]["222"]=account("222");
    auto id=makeChallenge();r["profiles"].push_back({{"id",id},{"owner","111"},{"world","fixture"},{"uid",4000000001u},
        {"name","网易角色"},{"source_identity_key","player_netease"},{"source_data_key","player_netease"},
        {"source_hash","source-hash"},{"expected_hash","copy-hash"},{"data_key","player_server_npm_"+id},{"status","ready"},
        {"netease_uuid","00000000-0000-4000-8000-000000000001"},{"error",""},{"created_at",123}});
    r["accounts"]["111"]["pending"]={{"id",makeChallenge()},{"profile_id",id},{"previous_key","player_server_original-111"},
        {"next_key","player_server_npm_"+id},{"status","pending"},{"created_at",124}};return r;
}
int main(int argc,char** argv){try{
    std::string error;
    if(argc==4&&std::string(argv[1])=="--migrate"){
        RegistryStore store;Registry r;require(store.open(argv[3],error),error);require(store.load(argv[2],r,error),error);
        std::cout<<"Registry DB ready: "<<r["accounts"].size()<<" accounts, "<<r["profiles"].size()<<" profiles\n";return 0;
    }
    auto root=std::filesystem::current_path()/".runtime"/("registry-tests-"+makeChallenge());std::filesystem::create_directories(root);
    auto legacy=root/"profiles.json",database=root/"profiles.sqlite3";auto original=sample();
    require(validateRegistry(original),"valid fixture");require(durableWrite(legacy,original.dump(2)),"write legacy fixture");
    {
        RegistryStore store;Registry loaded;require(store.open(database,error),error);require(store.load(legacy,loaded,error),error);
        require(loaded==original,"legacy import preserves ownership, optional fields and pending switch");
        auto backup=legacy;backup+=L".pre-sqlite.bak";require(readFile(backup)==readFile(legacy),"byte-exact legacy backup");
        require(durableWrite(legacy,emptyRegistry().dump()),"alter old JSON");
        require(store.load(legacy,loaded,error)&&loaded==original,"database is authoritative after first import");
        sql(database,"INSERT INTO uid_claims VALUES('fixture',4000000001,'222')",false);
        auto pid=original["profiles"][0]["id"].get<std::string>();
        sql(database,("UPDATE profiles SET owner='222' WHERE id='"+pid+"'").c_str(),false);
        sql(database,"INSERT INTO identity_aliases VALUES('fixture','111','222')",false);
        RegistryStore stale;Registry staleSnapshot;require(stale.open(database,error)&&stale.load(legacy,staleSnapshot,error),error);
        auto committed=loaded;committed["accounts"]["111"]["pending"]["status"]="applying";
        require(store.save(committed,error),error);
        require(!stale.save(staleSnapshot,error)&&error.find("stale")!=std::string::npos,"stale writer cannot replace committed state");
        Registry check;require(store.load(legacy,check,error)&&check==committed,"stale rejection preserves committed state");
        sql(database,"CREATE TRIGGER inject_failure BEFORE INSERT ON profiles BEGIN SELECT RAISE(ABORT,'injected test failure'); END");
        auto rejected=committed;rejected["profiles"][0]["name"]="must roll back";
        require(!store.save(rejected,error),"injected mid-transaction failure rejected");
        require(store.load(legacy,check,error)&&check==committed,"whole transaction rolls back, including deleted tables and revision");
        sql(database,"DROP TRIGGER inject_failure");
        auto invalid=committed;auto copy=invalid["profiles"][0];auto id=makeChallenge();copy["id"]=id;copy["owner"]="222";
        copy["data_key"]="player_server_npm_"+id;copy["source_data_key"]="player_other_source";invalid["profiles"].push_back(copy);
        require(!store.save(invalid,error),"duplicate UID owner rejected");
        auto complete=committed;complete["accounts"]["111"]["active_profile"]=pid;
        complete["accounts"]["111"]["last_switch"]=complete["accounts"]["111"]["pending"];complete["accounts"]["111"]["pending"]=nullptr;
        require(store.save(complete,error),error);original=complete;
    }
    {
        RegistryStore reopened;Registry loaded;require(reopened.open(database,error)&&reopened.load(legacy,loaded,error),error);
        require(loaded==original,"reopen preserves active profile and switch history");
    }
    auto badLegacy=root/"invalid.json",badDatabase=root/"invalid.sqlite3";
    require(durableWrite(badLegacy,"{broken"),"bad legacy fixture");
    {
        RegistryStore store;Registry loaded;require(store.open(badDatabase,error),error);
        require(!store.load(badLegacy,loaded,error),"invalid legacy import refused");
        require(durableWrite(badLegacy,sample().dump()),"repair test legacy file");
        require(store.load(badLegacy,loaded,error)&&loaded["profiles"].size()==1,"failed import can retry without discarding legacy");
    }
    auto corrupt=root/"corrupt.sqlite3";require(durableWrite(corrupt,"not a sqlite database"),"corrupt DB fixture");
    {RegistryStore store;require(!store.open(corrupt,error),"corrupt DB must not silently fall back to old JSON");}
    sql(database,"PRAGMA user_version=999");
    {RegistryStore store;require(!store.open(database,error),"unknown newer schema refused");}
    std::cout<<"Registry database tests passed: import, SQL exclusivity, persistence, stale writer and transaction rollback\n";
    return 0;
}catch(std::exception const& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}

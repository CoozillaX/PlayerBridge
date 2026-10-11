#include "migration/RegistryStore.h"
#include "migration/Files.h"
#include <sqlite3.h>
#include <stdexcept>
#include <map>
#include <vector>

namespace migration {
namespace {
constexpr int ApplicationId = 0x4e504d31;
using Fields = std::vector<char const*>;
Fields const accountFields{"world","uuid","msa_id","self_signed_id","identity_key","default_key","active_profile"};
Fields const profileFields{"id","owner","world","name","source_identity_key","source_data_key","source_hash","expected_hash","data_key","status"};
Fields const switchFields{"id","profile_id","previous_key","next_key","status"};
void exec(sqlite3* db, char const* sql) {
    if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
struct Statement {
    sqlite3* db;
    sqlite3_stmt* value = nullptr;
    Statement(sqlite3* d, char const* sql):db(d) {
        if(sqlite3_prepare_v2(db,sql,-1,&value,nullptr)!=SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement(){sqlite3_finalize(value);}
    void bind(int index, std::string const& text){
        if(sqlite3_bind_text(value,index,text.data(),static_cast<int>(text.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    void number(int index,int64_t n){
        if(sqlite3_bind_int64(value,index,n)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));
    }
    bool row(){int result=sqlite3_step(value);if(result==SQLITE_ROW)return true;
        if(result!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));return false;}
    void write(){if(row())throw std::runtime_error("Unexpected registry query row");}
    std::string text(int index){auto* p=sqlite3_column_text(value,index);if(!p)throw std::runtime_error("Null registry field");
        return {reinterpret_cast<char const*>(p),static_cast<size_t>(sqlite3_column_bytes(value,index))};}
    int64_t number(int index){return sqlite3_column_int64(value,index);}
};
struct Transaction {
    sqlite3* db;bool committed=false;
    Transaction(sqlite3* d,bool writing):db(d){exec(db,writing?"BEGIN IMMEDIATE":"BEGIN");}
    ~Transaction(){if(!committed)sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);}
    void commit(){exec(db,"COMMIT");committed=true;}
};
int64_t scalar(sqlite3* db,char const* sql){Statement q(db,sql);if(!q.row())throw std::runtime_error("Missing registry metadata");return q.number(0);}
Registry extras(Registry const& object,Fields const& fields){auto copy=object;for(auto f:fields)copy.erase(f);return copy;}
Registry readObject(Statement& q,int first,Fields const& fields){
    auto object=Registry::parse(q.text(first+static_cast<int>(fields.size())));
    if(!object.is_object())throw std::runtime_error("Invalid registry extension fields");
    for(size_t i=0;i<fields.size();++i){if(object.contains(fields[i]))throw std::runtime_error("Duplicated registry field");object[fields[i]]=q.text(first+static_cast<int>(i));}
    return object;
}
void bindObject(Statement& q,int first,Registry const& object,Fields const& fields,Registry extension){
    for(size_t i=0;i<fields.size();++i)q.bind(first+static_cast<int>(i),object.at(fields[i]).get<std::string>());
    q.bind(first+static_cast<int>(fields.size()),extension.dump());
}
char const* schema = R"SQL(
CREATE TABLE metadata(id INTEGER PRIMARY KEY CHECK(id=1),generation INTEGER NOT NULL,initialized INTEGER NOT NULL CHECK(initialized IN(0,1)));
INSERT INTO metadata VALUES(1,0,0);
CREATE TABLE accounts(
 xuid TEXT PRIMARY KEY NOT NULL,world TEXT NOT NULL,uuid TEXT NOT NULL,msa_id TEXT NOT NULL,self_signed_id TEXT NOT NULL,
 identity_key TEXT NOT NULL,default_key TEXT NOT NULL,active_profile TEXT NOT NULL,extra_json TEXT NOT NULL,
 UNIQUE(xuid,world),UNIQUE(world,msa_id),UNIQUE(world,default_key),CHECK(identity_key='player_'||msa_id));
CREATE TABLE identity_aliases(world TEXT NOT NULL,identity_id TEXT NOT NULL,owner TEXT NOT NULL,
 PRIMARY KEY(world,identity_id),FOREIGN KEY(owner,world) REFERENCES accounts(xuid,world));
CREATE TABLE uid_claims(world TEXT NOT NULL,uid INTEGER NOT NULL CHECK(uid BETWEEN 1 AND 4294967295),owner TEXT NOT NULL,
 PRIMARY KEY(world,uid),UNIQUE(world,uid,owner),FOREIGN KEY(owner,world) REFERENCES accounts(xuid,world));
CREATE TABLE profiles(
 id TEXT PRIMARY KEY NOT NULL,owner TEXT NOT NULL,world TEXT NOT NULL,name TEXT NOT NULL,source_identity_key TEXT NOT NULL,
 source_data_key TEXT NOT NULL,source_hash TEXT NOT NULL,expected_hash TEXT NOT NULL,data_key TEXT NOT NULL UNIQUE,
 status TEXT NOT NULL CHECK(status IN('adding','ready','failed')),extra_json TEXT NOT NULL,uid INTEGER NOT NULL,
 UNIQUE(world,source_data_key),UNIQUE(id,owner),CHECK(data_key='player_server_npm_'||id),
 FOREIGN KEY(world,uid,owner) REFERENCES uid_claims(world,uid,owner));
CREATE TABLE switches(
 owner TEXT PRIMARY KEY NOT NULL REFERENCES accounts(xuid),id TEXT NOT NULL UNIQUE,profile_id TEXT NOT NULL,
 previous_key TEXT NOT NULL,next_key TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN('pending','applying','failed')),
 extra_json TEXT NOT NULL,target_profile TEXT,
 CHECK((profile_id='default' AND target_profile IS NULL) OR (profile_id<>'default' AND target_profile=profile_id)),
 FOREIGN KEY(target_profile,owner) REFERENCES profiles(id,owner));
PRAGMA application_id=1313885489;
PRAGMA user_version=1;
)SQL";
}
RegistryStore::~RegistryStore(){if(mDb)sqlite3_close(mDb);}
bool RegistryStore::open(std::filesystem::path const& path,std::string& error){
    try{
        if(mDb)throw std::runtime_error("Registry database already open");
        std::filesystem::create_directories(path.parent_path());
        auto utf8=path.u8string();auto name=std::string(reinterpret_cast<char const*>(utf8.data()),utf8.size());
        if(sqlite3_open_v2(name.c_str(),&mDb,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK)
            throw std::runtime_error(mDb?sqlite3_errmsg(mDb):"Cannot open registry database");
        sqlite3_busy_timeout(mDb,5000);
        exec(mDb,"PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
        auto version=scalar(mDb,"PRAGMA user_version");
        if(version==0){
            if(scalar(mDb,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")!=0)
                throw std::runtime_error("Unrecognized registry database");
            Transaction tx(mDb,true);exec(mDb,schema);tx.commit();
        }else if(version!=1||scalar(mDb,"PRAGMA application_id")!=ApplicationId)throw std::runtime_error("Unsupported registry schema");
        Statement check(mDb,"PRAGMA quick_check");if(!check.row()||check.text(0)!="ok")throw std::runtime_error("Registry integrity check failed");
        mGeneration=scalar(mDb,"SELECT generation FROM metadata WHERE id=1");return true;
    }catch(std::exception const& e){error=e.what();if(mDb){sqlite3_close(mDb);mDb=nullptr;}return false;}
}
bool RegistryStore::load(std::filesystem::path const& legacyJson,Registry& result,std::string& error){
    try{
        if(!mDb)throw std::runtime_error("Registry database is not open");
        if(!scalar(mDb,"SELECT initialized FROM metadata WHERE id=1")){
            auto imported=emptyRegistry();
            if(std::filesystem::exists(legacyJson)){
                auto bytes=readFile(legacyJson);if(!bytes)throw std::runtime_error("Cannot read legacy registry");
                imported=Registry::parse(*bytes);if(!validateRegistry(imported))throw std::runtime_error("Invalid legacy registry; import refused");
                auto backup=legacyJson;backup+=L".pre-sqlite.bak";
                if(std::filesystem::exists(backup)){auto saved=readFile(backup);if(!saved||*saved!=*bytes)throw std::runtime_error("Legacy import backup differs; import refused");}
                else if(!durableWrite(backup,*bytes))throw std::runtime_error("Cannot back up legacy registry");
            }
            if(!save(imported,error))return false;
        }
        Transaction tx(mDb,false);auto loaded=emptyRegistry();
        auto generation=scalar(mDb,"SELECT generation FROM metadata WHERE id=1");
        Statement accounts(mDb,"SELECT xuid,world,uuid,msa_id,self_signed_id,identity_key,default_key,active_profile,extra_json FROM accounts ORDER BY xuid");
        while(accounts.row()){auto a=readObject(accounts,1,accountFields);if(a.contains("pending"))throw std::runtime_error("Pending field in account extensions");a["pending"]=nullptr;loaded["accounts"][accounts.text(0)]=std::move(a);}
        Statement profiles(mDb,"SELECT id,owner,world,name,source_identity_key,source_data_key,source_hash,expected_hash,data_key,status,extra_json,uid FROM profiles ORDER BY rowid");
        while(profiles.row()){auto p=readObject(profiles,0,profileFields);if(p.contains("uid"))throw std::runtime_error("UID in profile extensions");p["uid"]=static_cast<uint64_t>(profiles.number(11));loaded["profiles"].push_back(std::move(p));}
        Statement switches(mDb,"SELECT owner,id,profile_id,previous_key,next_key,status,extra_json FROM switches");
        while(switches.row()){auto owner=switches.text(0);if(!loaded["accounts"].contains(owner))throw std::runtime_error("Orphan switch owner");loaded["accounts"][owner]["pending"]=readObject(switches,1,switchFields);}
        Statement foreign(mDb,"PRAGMA foreign_key_check");if(foreign.row())throw std::runtime_error("Registry foreign key check failed");
        if(!validateRegistry(loaded))throw std::runtime_error("Database registry validation failed");
        tx.commit();result=std::move(loaded);mGeneration=generation;return true;
    }catch(std::exception const& e){error=e.what();return false;}
}
bool RegistryStore::save(Registry const& registry,std::string& error){
    try{
        if(!mDb)throw std::runtime_error("Registry database is not open");
        if(!validateRegistry(registry))throw std::runtime_error("Registry validation failed");
        Transaction tx(mDb,true);
        Statement revision(mDb,"UPDATE metadata SET generation=generation+1,initialized=1 WHERE id=1 AND generation=?");
        revision.number(1,mGeneration);revision.write();if(sqlite3_changes(mDb)!=1)throw std::runtime_error("Registry changed by another writer; stale update refused");
        exec(mDb,"DELETE FROM switches; DELETE FROM profiles; DELETE FROM uid_claims; DELETE FROM identity_aliases; DELETE FROM accounts;");
        for(auto const& [owner,a]:registry.at("accounts").items()){
            Statement q(mDb,"INSERT INTO accounts VALUES(?,?,?,?,?,?,?,?,?)");q.bind(1,owner);
            auto extra=extras(a,accountFields);extra.erase("pending");bindObject(q,2,a,accountFields,std::move(extra));q.write();
            auto msa=a.at("msa_id").get<std::string>(),self=a.at("self_signed_id").get<std::string>();
            Statement alias(mDb,"INSERT INTO identity_aliases VALUES(?,?,?)");alias.bind(1,a.at("world").get<std::string>());alias.bind(2,msa);alias.bind(3,owner);alias.write();
            if(!self.empty()&&self!=msa){Statement second(mDb,"INSERT INTO identity_aliases VALUES(?,?,?)");second.bind(1,a.at("world").get<std::string>());second.bind(2,self);second.bind(3,owner);second.write();}
        }
        std::map<std::pair<std::string,uint32_t>,std::string> claims;
        for(auto const& p:registry.at("profiles"))claims[{p.at("world").get<std::string>(),p.at("uid").get<uint32_t>()}]=p.at("owner").get<std::string>();
        for(auto const& [key,owner]:claims){Statement q(mDb,"INSERT INTO uid_claims VALUES(?,?,?)");q.bind(1,key.first);q.number(2,key.second);q.bind(3,owner);q.write();}
        for(auto const& p:registry.at("profiles")){
            Statement q(mDb,"INSERT INTO profiles VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");auto extra=extras(p,profileFields);extra.erase("uid");
            bindObject(q,1,p,profileFields,std::move(extra));q.number(12,p.at("uid").get<uint32_t>());q.write();
        }
        for(auto const& [owner,a]:registry.at("accounts").items())if(!a.at("pending").is_null()){
            auto const& pending=a.at("pending");Statement q(mDb,"INSERT INTO switches VALUES(?,?,?,?,?,?,?,?)");q.bind(1,owner);
            bindObject(q,2,pending,switchFields,extras(pending,switchFields));
            auto id=pending.at("profile_id").get<std::string>();if(id!="default")q.bind(8,id);q.write();
        }
        tx.commit();++mGeneration;return true;
    }catch(std::exception const& e){error=e.what();return false;}
}
}

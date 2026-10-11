#include "profiles/ProfileRegistry.h"
#include <map>
#include <set>

namespace profiles {
Registry emptyRegistry() {
    return {{"version", 2}, {"accounts", Registry::object()}, {"profiles", Registry::array()}};
}

bool safeProfileId(std::string_view id) {
    return id.size() == 28 && id.starts_with("PRF-")
        && id.substr(4).find_first_not_of("0123456789ABCDEF") == std::string_view::npos;
}

Registry const* ownedProfile(Registry const& r, std::string_view id, std::string_view xuid) {
    for (auto const& p : r.at("profiles")) {
        if (p.at("id").get_ref<std::string const&>() == id && p.at("owner").get_ref<std::string const&>() == xuid) {
            return &p;
        }
    }
    return nullptr;
}

bool mayClaim(
    Registry const&  r,
    std::string_view world,
    uint32_t         uid,
    std::string_view sourceKey,
    std::string_view xuid
) {
    for (auto const& p : r.at("profiles")) {
        if (p.at("world").get_ref<std::string const&>() != world) {
            continue;
        }
        if ((p.at("uid") == uid || p.at("source_data_key").get_ref<std::string const&>() == sourceKey)
            && p.at("owner").get_ref<std::string const&>() != xuid) {
            return false;
        }
    }
    return true;
}

bool validateRegistry(Registry const& r) {
    try {
        if (!r.is_object() || r.at("version") != 2 || !r.at("accounts").is_object() || !r.at("profiles").is_array()) {
            return false;
        }
        std::set<std::string>                                   ids, dataKeys, sourceKeys;
        std::map<std::pair<std::string, uint32_t>, std::string> uidOwners;
        for (auto const& p : r.at("profiles")) {
            for (auto field :
                 {"id",
                  "owner",
                  "world",
                  "name",
                  "source_identity_key",
                  "source_data_key",
                  "source_hash",
                  "expected_hash",
                  "data_key",
                  "status"}) {
                if (!p.at(field).is_string()) {
                    return false;
                }
            }
            auto id = p.at("id").get<std::string>(), owner = p.at("owner").get<std::string>(),
                 world = p.at("world").get<std::string>();
            auto key = p.at("data_key").get<std::string>(), source = p.at("source_data_key").get<std::string>();
            auto uid64 = p.at("uid").get<uint64_t>();
            if (!p.at("uid").is_number_unsigned() || !uid64 || uid64 > UINT32_MAX || owner.empty() || world.empty()
                || !r.at("accounts").contains(owner) || !safeProfileId(id) || key != "player_server_profiles_" + id
                || !source.starts_with("player_") || source.starts_with("player_server_profiles_")
                || !p.at("source_identity_key").get<std::string>().starts_with("player_") || !ids.insert(id).second
                || !dataKeys.insert(key).second || !sourceKeys.insert(world + "\n" + source).second) {
                return false;
            }
            if (r.at("accounts").at(owner).at("world") != world) {
                return false;
            }
            auto pair = std::make_pair(world, static_cast<uint32_t>(uid64));
            if (uidOwners.contains(pair) && uidOwners.at(pair) != owner) {
                return false;
            }
            uidOwners[pair] = owner;
            auto status     = p.at("status").get<std::string>();
            if (status != "adding" && status != "ready" && status != "failed") {
                return false;
            }
        }
        std::set<std::string> defaults;
        std::set<std::string> identityAliases;
        for (auto const& [xuid, a] : r.at("accounts").items()) {
            for (auto field :
                 {"world", "uuid", "msa_id", "self_signed_id", "identity_key", "default_key", "active_profile"}) {
                if (!a.at(field).is_string()) {
                    return false;
                }
            }
            auto key   = a.at("default_key").get<std::string>();
            auto world = a.at("world").get<std::string>(), msa = a.at("msa_id").get<std::string>(),
                 self = a.at("self_signed_id").get<std::string>();
            if (!identityAliases.insert(world + "\n" + msa).second
                || (!self.empty() && self != msa && !identityAliases.insert(world + "\n" + self).second)) {
                return false;
            }
            if (xuid.empty() || msa.empty() || a.at("identity_key").get<std::string>() != "player_" + msa
                || !key.starts_with("player_server_") || key.starts_with("player_server_profiles_")
                || !defaults.insert(a.at("world").get<std::string>() + "\n" + key).second || dataKeys.contains(key)
                || sourceKeys.contains(a.at("world").get<std::string>() + "\n" + key)) {
                return false;
            }
            auto active = a.at("active_profile").get<std::string>();
            if (active != "default") {
                auto* p = ownedProfile(r, active, xuid);
                if (!p || p->at("world") != a.at("world") || p->at("status") != "ready") {
                    return false;
                }
            }
            auto const& pending = a.at("pending");
            if (!pending.is_null()) {
                for (auto field : {"id", "profile_id", "previous_key", "next_key", "status"}) {
                    if (!pending.at(field).is_string()) {
                        return false;
                    }
                }
                if (!safeProfileId(pending.at("id").get<std::string>())) {
                    return false;
                }
                auto status = pending.at("status").get<std::string>();
                if (status != "pending" && status != "applying" && status != "failed") {
                    return false;
                }
                auto  selected = pending.at("profile_id").get<std::string>();
                auto* p        = selected == "default" ? nullptr : ownedProfile(r, selected, xuid);
                if (selected != "default" && (!p || p->at("status") != "ready" || p->at("world") != a.at("world"))) {
                    return false;
                }
                if (pending.at("next_key") != (p ? p->at("data_key") : a.at("default_key"))) {
                    return false;
                }
                auto* previous = active == "default" ? nullptr : ownedProfile(r, active, xuid);
                if (pending.at("previous_key") != (previous ? previous->at("data_key") : a.at("default_key"))) {
                    return false;
                }
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace profiles

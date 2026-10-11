#include "profiles/ApiClient.h"
#include "profiles/Files.h"
#include "profiles/Identity.h"
#include "profiles/NbtSanitizer.h"
#include "profiles/ProfileRegistry.h"
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

void require(bool condition, char const* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void append(std::string& s, uint32_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
        s += static_cast<char>(value >> (8 * i));
    }
}

void name(std::string& s, std::string_view value) {
    append(s, static_cast<uint32_t>(value.size()), 2);
    s += value;
}

int main(int argc, char** argv) {
    using namespace profiles;
    require(parseUid("4000000001") == 4000000001u, "large unsigned UID");
    for (auto bad : {"", "0", "-1", "+1", " 1", "1 ", "4294967296", "1e3"}) {
        require(!parseUid(bad), "reject invalid UID");
    }
    for (uint32_t value : {1u, 4000000001u, 4294967295u, 0x80000000u, 0u}) {
        auto encoded = encryptUid(value);
        require(decryptUid(encoded) == value, "Skip32 round trip");
        std::ostringstream hex;
        hex << std::hex << std::setw(8) << std::setfill('0') << encoded;
        auto uuid = "00000000-0000-4000-8000-0000" + hex.str();
        require(uuidMatchesUid(uuid, value), "big-endian UUID suffix");
        require(!uuidMatchesUid(uuid, value ^ 0x800u), "wrong UID rejected");
        std::string reversed;
        for (int i = 6; i >= 0; i -= 2) {
            reversed += hex.str().substr(static_cast<size_t>(i), 2);
        }
        require(uuidMatchesUid("00000000-0000-4000-8000-0000" + reversed, value), "little-endian UUID suffix");
    }
    require(!uuidMatchesUid("../anything", 1), "malformed UUID");
    require(trimSignature("PRF-ABC\r\n") == "PRF-ABC", "trailing newline normalization");
    require(trimSignature(" PRF-ABC ") == " PRF-ABC ", "spaces not silently accepted");
    require(makeChallenge().size() == 28 && makeChallenge() != makeChallenge(), "random challenges");
    for (int i = 0; i < 100; ++i) {
        auto code = makeVerificationCode();
        require(
            code.size() == 6 && code[0] != '0' && code.find_first_not_of("0123456789") == std::string::npos,
            "six digit manual verification code"
        );
    }
    std::string normal;
    normal += char(10);
    name(normal, "");
    normal += char(3);
    name(normal, "Health");
    append(normal, 20, 4);
    normal += char(0);
    require(sanitizePlayerNbt(normal) == normal, "standard NBT unchanged");
    auto source  = normal.substr(0, normal.size() - 1);
    source      += char(8);
    name(source, "scriptData");
    append(source, 3, 4);
    source += "abc";
    source += char(8);
    name(source, "saveLongString");
    name(source, "remove-me");
    source += char(0);
    require(sanitizePlayerNbt(source) == normal, "remove only NetEase root fields");
    for (size_t i = 0; i < source.size(); ++i) {
        require(!sanitizePlayerNbt(std::string_view(source).substr(0, i)), "truncated NBT rejected");
    }
    auto overflow  = normal.substr(0, normal.size() - 1);
    overflow      += char(8);
    name(overflow, "scriptData");
    append(overflow, 0xffffffffu, 4);
    overflow += char(0);
    require(!sanitizePlayerNbt(overflow), "oversized lengths rejected");
    require(!sanitizePlayerNbt(normal + "garbage"), "trailing bytes rejected");
    require(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "backup SHA256");
    auto registry = emptyRegistry();
    require(validateRegistry(registry), "empty registry");
    auto originalAccount = nlohmann::json{
        {"world", "fixture"},
        {"uuid", "uuid-a"},
        {"msa_id", "111"},
        {"self_signed_id", "uuid-a"},
        {"identity_key", "player_111"},
        {"default_key", "player_server_original-a"},
        {"active_profile", "default"},
        {"pending", nullptr}
    };
    registry["accounts"]["111"] = originalAccount;
    auto id                     = makeChallenge();
    auto profile                = nlohmann::json{
        {"id", id},
        {"owner", "111"},
        {"uid", 4000000001u},
        {"world", "fixture"},
        {"name", "fixture"},
        {"source_identity_key", "player_fixture-uuid"},
        {"source_data_key", "player_fixture-uuid"},
        {"source_hash", "hash"},
        {"expected_hash", "hash"},
        {"data_key", "player_server_profiles_" + id},
        {"status", "ready"}
    };
    registry["profiles"].push_back(profile);
    require(validateRegistry(registry), "original international slot and NetEase slot coexist");
    auto withoutAlias                                 = registry;
    withoutAlias["accounts"]["111"]["self_signed_id"] = "";
    require(validateRegistry(withoutAlias), "authenticated MSA login may omit optional SelfSignedId");
    require(mayClaim(registry, "fixture", 4000000001u, "new-role", "111"), "one owner may add another role");
    require(!mayClaim(registry, "fixture", 4000000001u, "new-role", "222"), "second international owner denied");
    require(!mayClaim(registry, "fixture", 12u, "player_fixture-uuid", "222"), "source alias cannot evade ownership");
    require(ownedProfile(registry, id, "111") && !ownedProfile(registry, id, "222"), "profile access checks owner");
    registry["accounts"]["111"]["pending"] = {
        {"id", makeChallenge()},
        {"profile_id", id},
        {"status", "pending"},
        {"previous_key", "player_server_original-a"},
        {"next_key", profile["data_key"]}
    };
    require(validateRegistry(registry), "pending switch keeps original slot");
    auto reloaded = nlohmann::json::parse(registry.dump());
    require(validateRegistry(reloaded) && reloaded == registry, "ownership and switch survive restart");
    reloaded["accounts"]["111"]["pending"]["next_key"] = "player_server_foreign";
    require(!validateRegistry(reloaded), "foreign target rejected on journal reload");
    auto second                  = originalAccount;
    second["msa_id"]             = "222";
    second["self_signed_id"]     = "uuid-b";
    second["identity_key"]       = "player_222";
    second["default_key"]        = "player_server_original-b";
    registry["accounts"]["222"]  = second;
    auto duplicate               = profile;
    auto otherId                 = makeChallenge();
    duplicate["id"]              = otherId;
    duplicate["data_key"]        = "player_server_profiles_" + otherId;
    duplicate["owner"]           = "222";
    duplicate["source_data_key"] = "player_other-role";
    registry["profiles"].push_back(duplicate);
    require(!validateRegistry(registry), "duplicate UID ownership rejected on load");
    require(!safeProfileId("../escape"), "profile ID cannot escape backup directory");
    std::cout << "Core tests passed\n";
    if (argc == 3) {
        auto text = readFile(argv[1]);
        require(text.has_value(), "read private API configuration");
        auto config = nlohmann::json::parse(*text, nullptr, false);
        require(config.is_object(), "parse API configuration");
        ApiConfig api{config.at("base_url").get<std::string>(), config.at("api_key").get<std::string>(), 15};
        auto      uid = parseUid(argv[2]);
        require(uid.has_value(), "API probe UID");
        auto result = fetchProfile(api, *uid);
        if (!result.success) {
            std::cerr << result.error << '\n';
            return 1;
        }
        std::cout << "API probe passed: matching UID, nickname and signature fields present\n";
    }
}

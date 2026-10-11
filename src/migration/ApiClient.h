#pragma once
#include <cstdint>
#include <string>

namespace migration {
struct ApiConfig {
    std::string baseUrl = "https://api.liliya233.uk";
    std::string apiKey;
    int timeoutSeconds = 15;
};
struct ProfileResult {
    bool success = false;
    std::string nickname;
    std::string signature;
    std::string error;
};
ProfileResult fetchProfile(ApiConfig const& config, uint32_t uid);
}

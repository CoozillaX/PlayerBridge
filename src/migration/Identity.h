#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace migration {
std::optional<uint32_t> parseUid(std::string_view text);
uint32_t encryptUid(uint32_t uid);
uint32_t decryptUid(uint32_t value);
// Both byte orders are checked against observed world records, never used to guess a key.
bool uuidMatchesUid(std::string_view uuid, uint32_t uid);
std::string makeChallenge();
std::string makeVerificationCode();
std::string trimSignature(std::string_view value);
}

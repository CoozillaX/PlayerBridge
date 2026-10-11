#include "profiles/Identity.h"
#include <array>
#include <charconv>

// Windows SDK extension headers require the base Windows types first.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

namespace profiles {
namespace {
constexpr std::array<uint8_t, 256> Table{
    163, 215, 9,   131, 248, 72,  246, 244, 179, 33,  21,  120, 153, 177, 175, 249, 231, 45,  77,  138, 206, 76,
    202, 46,  82,  149, 217, 30,  78,  56,  68,  40,  10,  223, 2,   160, 23,  241, 96,  104, 18,  183, 122, 195,
    233, 250, 61,  83,  150, 132, 107, 186, 242, 99,  154, 25,  124, 174, 229, 245, 247, 22,  106, 162, 57,  182,
    123, 15,  193, 147, 129, 27,  238, 180, 26,  234, 208, 145, 47,  184, 85,  185, 218, 133, 63,  65,  191, 224,
    90,  88,  128, 95,  102, 11,  216, 144, 53,  213, 192, 167, 51,  6,   101, 105, 69,  0,   148, 86,  109, 152,
    155, 118, 151, 252, 178, 194, 176, 254, 219, 32,  225, 235, 214, 228, 221, 71,  74,  29,  66,  237, 158, 110,
    73,  60,  205, 67,  39,  210, 7,   212, 222, 199, 103, 24,  137, 203, 48,  31,  141, 198, 143, 170, 200, 116,
    220, 201, 93,  92,  49,  164, 112, 136, 97,  44,  159, 13,  43,  135, 80,  130, 84,  100, 38,  125, 3,   64,
    52,  75,  28,  115, 209, 196, 253, 59,  204, 251, 127, 171, 230, 62,  91,  165, 173, 4,   35,  156, 20,  81,
    34,  240, 41,  121, 113, 126, 255, 140, 14,  226, 12,  239, 188, 114, 117, 111, 55,  161, 236, 211, 142, 98,
    139, 134, 16,  232, 8,   119, 17,  190, 146, 79,  36,  197, 50,  54,  157, 207, 243, 166, 187, 172, 94,  108,
    169, 19,  87,  37,  181, 227, 189, 168, 58,  1,   5,   89,  42,  70
};
constexpr std::string_view Key = "SaintSteve";

uint16_t roundG(int k, uint16_t w) {
    auto a = static_cast<uint8_t>(w >> 8), b = static_cast<uint8_t>(w);
    auto c = Table[b ^ Key[(4 * k) % 10]] ^ a;
    auto d = Table[c ^ Key[(4 * k + 1) % 10]] ^ b;
    auto e = Table[d ^ Key[(4 * k + 2) % 10]] ^ c;
    auto f = Table[e ^ Key[(4 * k + 3) % 10]] ^ d;
    return static_cast<uint16_t>((e << 8) | f);
}

uint32_t transform(uint32_t value, bool encrypt) {
    uint16_t a = static_cast<uint16_t>(value >> 16), b = static_cast<uint16_t>(value);
    int      k = encrypt ? 0 : 23, step = encrypt ? 1 : -1;
    for (int i = 0; i < 12; ++i) {
        b  = static_cast<uint16_t>(b ^ roundG(k, a) ^ k);
        k += step;
        a  = static_cast<uint16_t>(a ^ roundG(k, b) ^ k);
        k += step;
    }
    return (static_cast<uint32_t>(b) << 16) | a;
}
} // namespace

std::optional<uint32_t> parseUid(std::string_view text) {
    uint32_t value{};
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value == 0) {
        return {};
    }
    return value;
}

uint32_t encryptUid(uint32_t value) {
    return transform(value, true);
}

uint32_t decryptUid(uint32_t value) {
    return transform(value, false);
}

bool uuidMatchesUid(std::string_view uuid, uint32_t uid) {
    std::string hex;
    for (char c : uuid) {
        if (c != '-') {
            hex += c;
        }
    }
    if (hex.size() != 32) {
        return false;
    }
    // Require UUID v4 and the RFC variant, as in the referenced role UUID algorithm.
    if (hex[12] != '4'
        || (hex[16] != '8' && hex[16] != '9' && hex[16] != 'a' && hex[16] != 'b' && hex[16] != 'A' && hex[16] != 'B')) {
        return false;
    }
    uint32_t tail{};
    auto [end, ec] = std::from_chars(hex.data() + 24, hex.data() + 32, tail, 16);
    if (ec != std::errc{} || end != hex.data() + 32) {
        return false;
    }
    for (size_t i = 0; i < 24; ++i) {
        if (!((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f')
              || (hex[i] >= 'A' && hex[i] <= 'F'))) {
            return false;
        }
    }
    auto reversed = ((tail & 0xff) << 24) | ((tail & 0xff00) << 8) | ((tail >> 8) & 0xff00) | (tail >> 24);
    return decryptUid(tail) == uid || decryptUid(reversed) == uid;
}

std::string makeChallenge() {
    std::array<uint8_t, 12> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return {};
    }
    constexpr char hex[]  = "0123456789ABCDEF";
    std::string    result = "PRF-";
    for (auto b : bytes) {
        result += hex[b >> 4];
        result += hex[b & 15];
    }
    return result;
}

std::string makeVerificationCode() {
    // Six digits without leading zero; rejection avoids modulo bias.
    constexpr uint32_t range = 900000;
    constexpr uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    uint32_t           value{};
    do {
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG)
            < 0) {
            return {};
        }
    } while (value >= limit);
    return std::to_string(100000 + value % range);
}

std::string trimSignature(std::string_view value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return std::string(value);
}
} // namespace profiles

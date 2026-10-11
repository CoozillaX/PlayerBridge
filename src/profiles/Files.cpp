#include "profiles/Files.h"
#include <array>
#include <fstream>
#include <iterator>
#include <vector>

// Windows SDK extension headers require the base Windows types first.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

namespace profiles {
bool durableWrite(std::filesystem::path const& file, std::string_view bytes) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        return false;
    }
    auto temp  = file;
    temp      += L".tmp";
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written{};
    bool  ok = bytes.size() <= MAXDWORD
            && WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
            && written == bytes.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) {
        return false;
    }
    return MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::optional<std::string> readFile(std::filesystem::path const& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::string value((std::istreambuf_iterator<char>(stream)), {});
    if (stream.bad()) {
        return {};
    }
    return value;
}

std::string sha256(std::string_view bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return {};
    }
    DWORD length{}, read{};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &read, 0)
        < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<uint8_t>    object(length);
    BCRYPT_HASH_HANDLE      hash{};
    std::array<uint8_t, 32> digest{};
    bool                    ok = BCryptCreateHash(algorithm, &hash, object.data(), length, nullptr, 0, 0) >= 0;
    if (ok) {
        ok = bytes.size() <= MAXDWORD
          && BCryptHashData(
                 hash,
                 reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                 static_cast<ULONG>(bytes.size()),
                 0
             ) >= 0;
    }
    if (ok) {
        ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    }
    if (hash) {
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) {
        return {};
    }
    std::string    result;
    constexpr char hex[] = "0123456789abcdef";
    for (auto b : digest) {
        result += hex[b >> 4];
        result += hex[b & 15];
    }
    return result;
}
} // namespace profiles

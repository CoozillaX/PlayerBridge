#include "profiles/NbtSanitizer.h"
#include <cstdint>
#include <vector>

namespace profiles {
namespace {
struct Reader {
    std::string_view data;
    size_t           pos{};

    bool take(size_t count) {
        if (count > data.size() - pos) {
            return false;
        }
        pos += count;
        return true;
    }

    bool integer(size_t count, uint32_t& value) {
        if (count > data.size() - pos) {
            return false;
        }
        value = 0;
        for (size_t i = 0; i < count; ++i) {
            value |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + i])) << (8 * i);
        }
        pos += count;
        return true;
    }

    bool text(std::string_view& value) {
        uint32_t size{};
        if (!integer(2, size) || size > data.size() - pos) {
            return false;
        }
        value  = data.substr(pos, size);
        pos   += size;
        return true;
    }

    bool payload(uint32_t type, size_t depth) {
        if (depth > 64) {
            return false;
        }
        uint32_t         length{}, child{};
        std::string_view name;
        switch (type) {
        case 1:
            return take(1);
        case 2:
            return take(2);
        case 3:
        case 5:
            return take(4);
        case 4:
        case 6:
            return take(8);
        case 7:
        case 11:
        case 12:
            if (!integer(4, length) || length > 0x7fffffff) {
                return false;
            }
            return take(static_cast<size_t>(length) * (type == 7 ? 1 : type == 11 ? 4 : 8));
        case 8:
            return text(name);
        case 9:
            if (!integer(1, child) || !integer(4, length) || length > 1000000 || child > 12
                || (child == 0 && length != 0)) {
                return false;
            }
            for (uint32_t i = 0; i < length; ++i) {
                if (!payload(child, depth + 1)) {
                    return false;
                }
            }
            return true;
        case 10:
            for (;;) {
                if (!integer(1, child)) {
                    return false;
                }
                if (!child) {
                    return true;
                }
                if (!text(name) || !payload(child, depth + 1)) {
                    return false;
                }
            }
        default:
            return false;
        }
    }
};
} // namespace

std::optional<std::string> sanitizePlayerNbt(std::string_view data) {
    if (data.size() < 4 || data.size() > 32 * 1024 * 1024) {
        return {};
    }
    Reader           reader{data};
    uint32_t         type{};
    std::string_view name;
    if (!reader.integer(1, type) || type != 10 || !reader.text(name)) {
        return {};
    }
    std::string output(data.substr(0, reader.pos));
    for (;;) {
        size_t start = reader.pos;
        if (!reader.integer(1, type)) {
            return {};
        }
        if (!type) {
            output.push_back('\0');
            break;
        }
        if (!reader.text(name)) {
            return {};
        }
        if (name == "scriptData") {
            // NetEase encodes this opaque root field with a 32-bit byte length.
            uint32_t length{};
            if (!reader.integer(4, length) || length > 0x7fffffff || !reader.take(length)) {
                return {};
            }
        } else {
            if (!reader.payload(type, 1)) {
                return {};
            }
            if (name != "saveLongString") {
                output.append(data.substr(start, reader.pos - start));
            }
        }
    }
    if (reader.pos != data.size()) {
        return {};
    }
    return output;
}
} // namespace profiles

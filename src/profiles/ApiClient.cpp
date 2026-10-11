#include "profiles/ApiClient.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <winhttp.h>

namespace profiles {
namespace {
struct Handle {
    HINTERNET value{};

    explicit Handle(HINTERNET v) : value(v) {}

    ~Handle() {
        if (value) {
            WinHttpCloseHandle(value);
        }
    }

    Handle(Handle const&)            = delete;
    Handle& operator=(Handle const&) = delete;
};

std::wstring wide(std::string const& value) {
    int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        size
    );
    return output;
}

ProfileResult fail(std::string error) {
    return {false, {}, {}, std::move(error)};
}
} // namespace

ProfileResult fetchProfile(ApiConfig const& config, uint32_t uid) {
    if (config.apiKey.empty() || config.apiKey.find_first_of("\r\n") != std::string::npos) {
        return fail("API key 未配置或格式无效");
    }
    auto url = wide(config.baseUrl);
    while (!url.empty() && url.back() == L'/') {
        url.pop_back();
    }
    url += L"/openapi/helper/get_user_details";
    URL_COMPONENTS parts{};
    parts.dwStructSize      = sizeof(parts);
    parts.dwHostNameLength  = static_cast<DWORD>(-1);
    parts.dwUrlPathLength   = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS
        || parts.dwExtraInfoLength != 0) {
        return fail("API 地址必须使用 HTTPS，且不包含查询参数");
    }
    Handle session(WinHttpOpen(L"NeteasePlayerBridge/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
    if (!session.value) {
        return fail("无法创建 HTTP 会话");
    }
    auto timeout = std::clamp(config.timeoutSeconds, 1, 60) * 1000;
    WinHttpSetTimeouts(session.value, timeout, timeout, timeout, timeout);
    auto   host = std::wstring(parts.lpszHostName, parts.dwHostNameLength);
    Handle connection(WinHttpConnect(session.value, host.c_str(), parts.nPort, 0));
    if (!connection.value) {
        return fail("无法连接 API");
    }
    auto   path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
    Handle request(
        WinHttpOpenRequest(connection.value, L"POST", path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE)
    );
    if (!request.value) {
        return fail("无法创建 API 请求");
    }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    auto headers = wide("Content-Type: application/json\r\nAuthorization: " + config.apiKey + "\r\n");
    auto body    = nlohmann::json{{"uid", uid}}.dump();
    if (!WinHttpSendRequest(
            request.value,
            headers.c_str(),
            static_cast<DWORD>(headers.size()),
            body.data(),
            static_cast<DWORD>(body.size()),
            static_cast<DWORD>(body.size()),
            0
        )
        || !WinHttpReceiveResponse(request.value, nullptr)) {
        return fail("API 请求失败或超时，请稍后重试");
    }
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request.value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            nullptr,
            &status,
            &size,
            nullptr
        )
        || status != 200) {
        return fail("API 返回非成功 HTTP 状态");
    }
    std::string response;
    for (;;) {
        DWORD available{};
        if (!WinHttpQueryDataAvailable(request.value, &available)) {
            return fail("读取 API 响应失败");
        }
        if (!available) {
            break;
        }
        if (response.size() + available > 1024 * 1024) {
            return fail("API 响应过大");
        }
        auto old = response.size();
        response.resize(old + available);
        DWORD read{};
        if (!WinHttpReadData(request.value, response.data() + old, available, &read)) {
            return fail("读取 API 响应失败");
        }
        response.resize(old + read);
    }
    auto json = nlohmann::json::parse(response, nullptr, false);
    if (!json.is_object() || !json.contains("success") || !json["success"].is_boolean()
        || !json["success"].get<bool>()) {
        return fail("API 未返回成功结果");
    }
    if (!json.contains("data") || !json["data"].is_object()) {
        return fail("API 玩家数据格式不正确");
    }
    auto const& data = json["data"];
    if (!data.contains("id") || !data["id"].is_number_unsigned() || data["id"].get<uint64_t>() != uid
        || !data.contains("signature") || !data["signature"].is_string() || !data.contains("nickname")
        || !data["nickname"].is_string()) {
        return fail("API 玩家身份或签名字段无效");
    }
    return {true, data["nickname"].get<std::string>(), data["signature"].get<std::string>(), {}};
}
} // namespace profiles

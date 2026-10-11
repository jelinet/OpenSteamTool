#include "CDKeyReporter.h"

#include "OSTPlatform/include/Http.h"
#include "Utils/Logging/Log.h"

#include <cstdio>

namespace CDKeyReporter {
namespace {

    // TODO: 上报地址（POST，body 为 JSON）。
    constexpr const char* kReportUrl = "https://example.com/api/cdkey/report";

    // TODO: 需要的话加上鉴权等请求头，每行以 \r\n 结尾。
    constexpr const wchar_t* kHeaders = L"Content-Type: application/json\r\n";

    // TODO: 超时（毫秒）。同步上报会让 Steam 网络线程停下来等，
    // 解析 / 连接 / 发送 / 接收各用这个值，最坏情况约 4 倍。不要设太大。
    constexpr uint32_t kTimeoutMs = 2000;

    // EPurchaseResultDetail 中和 CDKey 激活相关的常见值
    // （完整列表见 SteamKit 的 enums.steamd）。
    constexpr int32_t kDetailAlreadyPurchased        = 9;    // 账号已拥有该游戏
    constexpr int32_t kDetailRestrictedCountry       = 13;   // 锁区
    constexpr int32_t kDetailBadActivationCode       = 14;   // 无效 key
    constexpr int32_t kDetailDuplicateActivationCode = 15;   // key 已被其他账号激活
    constexpr int32_t kDetailDoesNotOwnRequiredApp   = 24;   // 需要先拥有本体
    constexpr int32_t kDetailRateLimited             = 53;   // 尝试次数过多

    constexpr int32_t kEResultOK = 1;

    // 转义 JSON 字符串中的特殊字符。
    std::string JsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (unsigned char c : s) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out;
    }

    // TODO: 上报的 JSON 格式，按你的接口修改。当前格式：
    //   {"key":"XXXXX-XXXXX-XXXXX","steamid":"7656...","eresult":1,"detail":0,"status":"activated"}
    // steamid 用字符串，避免 JS 等语言里 64 位整数丢精度。
    std::string BuildJson(const Report& r) {
        std::string json = "{\"key\":\"" + JsonEscape(r.key) + "\"";
        json += ",\"steamid\":\"" + std::to_string(r.steamId) + "\"";
        json += ",\"eresult\":" + std::to_string(r.eresult);
        json += ",\"detail\":" + std::to_string(r.detail);
        json += ",\"status\":\"";
        json += Status(r);
        json += "\"}";
        return json;
    }

} // namespace

const char* Status(const Report& r) {
    if (r.eresult == kEResultOK) return "activated";   // 本次激活成功
    switch (r.detail) {
    case kDetailDuplicateActivationCode: return "used_by_other_account";
    case kDetailAlreadyPurchased:        return "already_owned";
    case kDetailBadActivationCode:       return "invalid_key";
    case kDetailRestrictedCountry:       return "region_locked";
    case kDetailDoesNotOwnRequiredApp:   return "requires_base_game";
    case kDetailRateLimited:             return "rate_limited";
    default:                             return "failed";
    }
}

std::string MaskKey(const std::string& key) {
    constexpr size_t kVisible = 5;
    if (key.size() <= kVisible) return std::string(key.size(), '*');
    return std::string(key.size() - kVisible, '*') + key.substr(key.size() - kVisible);
}

bool SendReport(const Report& r) {
    // 同步上报：在调用方线程（Steam 网络线程）里直接发请求，返回后才继续处理这个包。
    const std::string body = BuildJson(r);
    auto res = OSTPlatform::Http::Execute(L"POST", kReportUrl,
                                          body.data(), static_cast<uint32_t>(body.size()),
                                          kHeaders,
                                          kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    if (!res.ok || res.status < 200 || res.status >= 300) {
        // 失败只写日志，不重试，不影响 Steam 的激活流程。
        LOG_NETPACKET_WARN("CDKey report failed key={} status={} ok={}",
                           MaskKey(r.key), res.status, res.ok);
        return false;
    }
    LOG_NETPACKET_INFO("CDKey report sent key={} status={}", MaskKey(r.key), Status(r));
    return true;
}

} // namespace CDKeyReporter

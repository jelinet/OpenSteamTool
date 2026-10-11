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

    // 标准 base64 编码，用来把二进制原始字节放进 JSON。
    std::string Base64(const std::string& in) {
        static constexpr char kTable[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((in.size() + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
            out += kTable[(v >> 18) & 63];
            out += kTable[(v >> 12) & 63];
            out += kTable[(v >> 6) & 63];
            out += kTable[v & 63];
        }
        if (i + 1 == in.size()) {
            const uint32_t v = uint8_t(in[i]) << 16;
            out += kTable[(v >> 18) & 63];
            out += kTable[(v >> 12) & 63];
            out += "==";
        } else if (i + 2 == in.size()) {
            const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
            out += kTable[(v >> 18) & 63];
            out += kTable[(v >> 12) & 63];
            out += kTable[(v >> 6) & 63];
            out += '=';
        }
        return out;
    }

    // TODO: 上报的 JSON 格式，按你的接口修改。当前格式：
    //   {
    //     "key": "XXXXX-XXXXX-XXXXX", "steamid": "7656...",
    //     "eresult": 1, "detail": 0, "status": "activated",
    //     "steam_response": {                       // Steam 763 响应的原样内容
    //       "header_eresult": 1,                    // header 里的 eresult
    //       "header_error_message": "",             // header 里的 error_message
    //       "eresult": 1,                           // body 里的 eresult
    //       "purchase_result_details": 0,           // body 里的失败原因
    //       "purchase_receipt_info": "base64...",   // body 里的购买收据（二进制，base64）
    //       "raw_header": "base64...",              // 原始 header 字节（base64）
    //       "raw_body": "base64..."                 // 原始 body 字节（base64）
    //     }
    //   }
    // raw_header / raw_body 是收到的原始字节，用 protobuf 按
    // CMsgProtoBufHeader / CMsgClientPurchaseResponse 解码即可还原全部字段。
    // steamid 用字符串，避免 JS 等语言里 64 位整数丢精度。
    //
    // ── 成功示例（本次激活成功，游戏已加到账号）──
    //   {
    //     "key": "AAAAA-BBBBB-CCCCC",
    //     "steamid": "76561198000000000",
    //     "eresult": 1,                          // 1 = k_EResultOK
    //     "detail": 0,                           // 成功时为 0（没有失败原因）
    //     "status": "activated",
    //     "steam_response": {
    //       "header_eresult": 1,
    //       "header_error_message": "",
    //       "eresult": 1,
    //       "purchase_result_details": 0,
    //       "purchase_receipt_info": "AHBhY2thZ2Vz...",  // 成功时一般有内容：激活到的
    //                                                    // package 等信息（二进制 KeyValues）
    //       "raw_header": "CQAAAAAAAAAA...",
    //       "raw_body": "CAEQABq..."
    //     }
    //   }
    //
    // ── 失败示例（key 已被其他账号激活）──
    //   {
    //     "key": "AAAAA-BBBBB-CCCCC",
    //     "steamid": "76561198000000000",
    //     "eresult": 2,                          // 不等于 1 就是失败，常见为 2 = k_EResultFail
    //     "detail": 15,                          // EPurchaseResultDetail，失败原因
    //     "status": "used_by_other_account",
    //     "steam_response": {
    //       "header_eresult": 2,
    //       "header_error_message": "",          // 一般为空，Steam 有时会带说明
    //       "eresult": 2,
    //       "purchase_result_details": 15,
    //       "purchase_receipt_info": "",         // 失败时一般为空
    //       "raw_header": "CQAAAAAAAAAA...",
    //       "raw_body": "CAIQDw=="
    //     }
    //   }
    //
    // 失败时 detail / status 的对应关系（其它值 status 都是 "failed"，看 detail 判断）：
    //   detail  9 -> "already_owned"          账号已拥有该游戏
    //   detail 13 -> "region_locked"          锁区，当前地区不能激活
    //   detail 14 -> "invalid_key"            无效 key
    //   detail 15 -> "used_by_other_account"  key 已被其他账号激活
    //   detail 24 -> "requires_base_game"     需要先拥有本体（DLC key）
    //   detail 53 -> "rate_limited"           尝试次数过多，稍后再试
    //
    // 注意：以上示例里的 base64 和 header_eresult 只是示意，没有用真实抓包验证。
    // 判断成功与否以 body 里的 eresult == 1 为准（就是外层的 "eresult"）。
    std::string BuildJson(const Report& r) {
        std::string json = "{\"key\":\"" + JsonEscape(r.key) + "\"";
        json += ",\"steamid\":\"" + std::to_string(r.steamId) + "\"";
        json += ",\"eresult\":" + std::to_string(r.eresult);
        json += ",\"detail\":" + std::to_string(r.detail);
        json += ",\"status\":\"";
        json += Status(r);
        json += "\"";
        json += ",\"steam_response\":{";
        json += "\"header_eresult\":" + std::to_string(r.headerEresult);
        json += ",\"header_error_message\":\"" + JsonEscape(r.headerErrorMessage) + "\"";
        json += ",\"eresult\":" + std::to_string(r.eresult);
        json += ",\"purchase_result_details\":" + std::to_string(r.detail);
        json += ",\"purchase_receipt_info\":\"" + Base64(r.receiptInfo) + "\"";
        json += ",\"raw_header\":\"" + Base64(r.rawHeader) + "\"";
        json += ",\"raw_body\":\"" + Base64(r.rawBody) + "\"";
        json += "}}";
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

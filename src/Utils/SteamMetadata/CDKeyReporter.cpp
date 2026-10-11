#include "CDKeyReporter.h"

#include "OSTPlatform/include/Http.h"
#include "Utils/Logging/Log.h"

#include <cstdio>

// 读注册表里的 MachineGuid 用
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace CDKeyReporter {
namespace {

    // TODO: 上报地址（POST，body 为 JSON）。
    constexpr const char* kReportUrl = "https://example.com/api/cdkey/report";

    // TODO: 激活前的机器码校验地址（POST，body 为 JSON，返回格式见 CDKeyReporter.h 的 CheckResponse）。
    // 校验失败（连不上 / 超时 / 非 2xx / 格式不对）都会拦下激活，所以这个接口要稳定、响应要快。
    constexpr const char* kCheckUrl = "https://example.com/api/cdkey/check";

    // TODO: 需要的话加上鉴权等请求头，每行以 \r\n 结尾（上报和校验共用）。
    constexpr const wchar_t* kHeaders = L"Content-Type: application/json\r\n";

    // TODO: 超时（毫秒，上报和校验共用）。同步上报会让 Steam 网络线程停下来等，
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

    // ── 解析接口返回的 JSON（手写，不加依赖）──
    // 限制：只取第一次出现的同名字段，不区分嵌套对象；不解码 \uXXXX（原样保留）。

    // 找到 "name" 后面冒号之后第一个非空白字符的位置，找不到返回 npos。
    size_t FindValue(const std::string& json, const char* name) {
        const std::string needle = std::string("\"") + name + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) return std::string::npos;
        pos = json.find_first_not_of(" \t\r\n", pos + needle.size());
        if (pos == std::string::npos || json[pos] != ':') return std::string::npos;
        return json.find_first_not_of(" \t\r\n", pos + 1);
    }

    // 读取整数字段，也兼容写成字符串的数字（"1"）。
    bool GetInt(const std::string& json, const char* name, int32_t& out) {
        size_t pos = FindValue(json, name);
        if (pos == std::string::npos) return false;
        if (json[pos] == '"') ++pos;
        bool neg = false;
        if (pos < json.size() && json[pos] == '-') { neg = true; ++pos; }
        if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') return false;
        int64_t v = 0;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            v = v * 10 + (json[pos++] - '0');
            if (v > INT32_MAX) return false;
        }
        out = static_cast<int32_t>(neg ? -v : v);
        return true;
    }

    // 读取字符串字段，处理常见转义（\" \\ \/ \n \r \t）。
    bool GetString(const std::string& json, const char* name, std::string& out) {
        size_t pos = FindValue(json, name);
        if (pos == std::string::npos || json[pos] != '"') return false;
        out.clear();
        for (++pos; pos < json.size(); ++pos) {
            char c = json[pos];
            if (c == '"') return true;
            if (c == '\\' && pos + 1 < json.size()) {
                char e = json[++pos];
                switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': out += "\\u"; break;   // 不解码，原样保留
                default:  out += e;      break;  // \" \\ \/
                }
            } else {
                out += c;
            }
        }
        return false;   // 字符串没有结束引号
    }

    // 读取布尔字段，只认 true / false。
    bool GetBool(const std::string& json, const char* name, bool& out) {
        size_t pos = FindValue(json, name);
        if (pos == std::string::npos) return false;
        if (json.compare(pos, 4, "true") == 0)  { out = true;  return true; }
        if (json.compare(pos, 5, "false") == 0) { out = false; return true; }
        return false;
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

ServerResponse SendReport(const Report& r) {
    // 步骤 1：同步上报。在调用方线程（Steam 网络线程）里直接发请求，返回后才继续处理这个包。
    const std::string body = BuildJson(r);
    auto res = OSTPlatform::Http::Execute(L"POST", kReportUrl,
                                          body.data(), static_cast<uint32_t>(body.size()),
                                          kHeaders,
                                          kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    ServerResponse out;
    out.httpStatus = res.status;
    out.httpOk     = res.ok && res.status >= 200 && res.status < 300;
    out.rawBody    = res.body;

    // 步骤 2：HTTP 失败（网络错误、超时、非 2xx）只写日志，不重试，不影响 Steam 的激活流程。
    if (!out.httpOk) {
        LOG_NETPACKET_WARN("CDKey report failed key={} status={} ok={} body={}",
                           MaskKey(r.key), res.status, res.ok, res.body);
        return out;
    }

    // 步骤 3：按 Steam 763 的字段解析接口返回的 JSON。
    //   eresult 必须有，没有就当作格式不对；另外两个字段可选。
    out.parsed = GetInt(res.body, "eresult", out.eresult);
    if (!out.parsed) {
        LOG_NETPACKET_WARN("CDKey report sent key={} but response has no eresult, body={}",
                           MaskKey(r.key), res.body);
        return out;
    }
    GetInt(res.body, "purchase_result_details", out.detail);
    GetString(res.body, "error_message", out.errorMessage);

    // 步骤 4：写日志。同时打出 Steam 的结果，方便对比两边是否一致。
    LOG_NETPACKET_INFO("CDKey report sent key={} steam: eresult={} detail={} | "
                       "server: eresult={} detail={} error_message={}",
                       MaskKey(r.key), r.eresult, r.detail,
                       out.eresult, out.detail, out.errorMessage);
    return out;
}

std::string GetMachineId() {
    // 只读一次，之后用缓存（机器码在 Steam 运行期间不会变）。
    static const std::string cached = [] {
        wchar_t buf[128] = {};
        DWORD bytes = sizeof(buf);
        // RRF_SUBKEY_WOW6464KEY：32 位进程也读 64 位注册表视图，
        // 否则 32 位 Steam 里会被重定向，读不到 MachineGuid。
        const LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE,
                                        L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                                        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                                        nullptr, buf, &bytes);
        if (st != ERROR_SUCCESS) {
            LOG_NETPACKET_WARN("CDKey: failed to read MachineGuid ({})", static_cast<long>(st));
            return std::string();
        }
        // MachineGuid 是 GUID 字符串，只有 ASCII，直接逐字符转换。
        std::string out;
        for (const wchar_t* p = buf; *p; ++p) out += static_cast<char>(*p);
        return out;
    }();
    return cached;
}

CheckResponse CheckBeforeActivate(const std::string& key, uint64_t steamId) {
    // 步骤 1：组装请求 JSON（TODO: 需要别的字段就在这里加），同步 POST 到校验接口。
    std::string body = "{\"key\":\"" + JsonEscape(key) + "\"";
    body += ",\"steamid\":\"" + std::to_string(steamId) + "\"";
    body += ",\"machine_id\":\"" + JsonEscape(GetMachineId()) + "\"}";
    auto res = OSTPlatform::Http::Execute(L"POST", kCheckUrl,
                                          body.data(), static_cast<uint32_t>(body.size()),
                                          kHeaders,
                                          kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    CheckResponse out;
    out.httpStatus = res.status;
    out.httpOk     = res.ok && res.status >= 200 && res.status < 300;
    out.rawBody    = res.body;

    // 步骤 2：HTTP 失败（网络错误、超时、非 2xx）一律拒绝。
    if (!out.httpOk) {
        LOG_NETPACKET_WARN("CDKey check failed key={} status={} ok={} -> deny",
                           MaskKey(key), res.status, res.ok);
        return out;
    }

    // 步骤 3：解析 allow。没有 allow 字段也当作拒绝。
    bool allow = false;
    if (!GetBool(res.body, "allow", allow)) {
        LOG_NETPACKET_WARN("CDKey check key={} response has no allow, body={} -> deny",
                           MaskKey(key), res.body);
        return out;
    }
    out.allow = allow;

    // 步骤 4：拒绝时读取失败原因（可选字段）。
    if (!allow) {
        int32_t detail = 0;
        if (GetInt(res.body, "purchase_result_details", detail)) out.detail = detail;
        GetString(res.body, "error_message", out.errorMessage);
    }

    LOG_NETPACKET_INFO("CDKey check key={} allow={} detail={} error_message={}",
                       MaskKey(key), out.allow, out.detail, out.errorMessage);
    return out;
}

} // namespace CDKeyReporter

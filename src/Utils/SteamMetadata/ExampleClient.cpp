#include "ExampleClient.h"

#include "OSTPlatform/include/Http.h"
#include "Utils/Logging/Log.h"

#include <charconv>
#include <cstdio>
#include <string_view>

namespace ExampleClient {
namespace {

    // TODO: 请求地址。%u 会被替换成 app id。
    constexpr const char* kUrlTemplate = "https://example.com/api/example/%u";

    // ── 简易 JSON 取值（不引入第三方库，写法同 ManifestClient） ──
    // 只按 "key" 查找第一次出现的位置，不处理嵌套对象里的同名字段，
    // 也不解码 \uXXXX。返回结构简单时够用；结构复杂时请自行调整。

    // 定位到 "key": 之后第一个非空白字符，找不到返回 npos。
    size_t FindValue(std::string_view body, std::string_view key) {
        std::string needle;
        needle.reserve(key.size() + 2);
        needle += '"';
        needle += key;
        needle += '"';

        size_t pos = body.find(needle);
        if (pos == std::string_view::npos) return pos;
        pos += needle.size();

        auto skipWs = [&] {
            while (pos < body.size() &&
                   (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\r' || body[pos] == '\n'))
                ++pos;
        };
        skipWs();
        if (pos >= body.size() || body[pos] != ':') return std::string_view::npos;
        ++pos;
        skipWs();
        return pos < body.size() ? pos : std::string_view::npos;
    }

    // 读取无符号整数，兼容 123 和 "123" 两种写法。
    bool GetUInt64(std::string_view body, std::string_view key, uint64_t* out) {
        size_t pos = FindValue(body, key);
        if (pos == std::string_view::npos) return false;
        if (body[pos] == '"') ++pos;
        const char* begin = body.data() + pos;
        const char* end   = body.data() + body.size();
        auto [ptr, ec] = std::from_chars(begin, end, *out);
        return ec == std::errc{} && ptr != begin;
    }

    // 读取字符串，处理 \" \\ \/ \n \t \r 这几种常见转义。
    bool GetString(std::string_view body, std::string_view key, std::string* out) {
        size_t pos = FindValue(body, key);
        if (pos == std::string_view::npos || body[pos] != '"') return false;

        out->clear();
        for (++pos; pos < body.size(); ++pos) {
            char c = body[pos];
            if (c == '"') return true;
            if (c == '\\' && pos + 1 < body.size()) {
                switch (body[++pos]) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default:  c = body[pos]; break;   // \" \\ \/ 以及其它原样保留
                }
            }
            out->push_back(c);
        }
        return false;   // 没有找到结束引号
    }

    // TODO: 把 JSON 响应映射到 Result。当前按下面的格式解析：
    //   { "value": 123456, "text": "hello" }
    // value 为必填（缺少则视为失败），text 为可选。
    bool ParseResponse(std::string_view body, Result* out) {
        if (!GetUInt64(body, "value", &out->value)) return false;
        GetString(body, "text", &out->text);
        return true;
    }

} // namespace

bool Fetch(AppId_t appId, Result* out) {
    if (!out) return false;

    char url[256];
    std::snprintf(url, sizeof(url), kUrlTemplate, appId);

    // 这里用 GET。如果要 POST 一个 JSON body，可以这样写：
    //   char req[64];
    //   int n = std::snprintf(req, sizeof(req), "{\"app_id\":%u}", appId);
    //   auto r = OSTPlatform::Http::Execute(L"POST", url, req, static_cast<uint32_t>(n),
    //                                       L"Content-Type: application/json\r\n");
    auto r = OSTPlatform::Http::Execute(L"GET", url, nullptr, 0, L"Accept: application/json\r\n");
    LOG_NETPACKET_INFO("Example HTTP status={} appid={}", r.status, appId);

    // r.ok 只表示拿到了 HTTP 响应，还需要自己检查状态码。
    if (!r.ok || r.status != 200) {
        LOG_NETPACKET_WARN("Example HTTP failed appid={} status={} ok={}", appId, r.status, r.ok);
        return false;
    }

    if (!ParseResponse(r.body, out)) {
        LOG_NETPACKET_WARN("Example HTTP returned unexpected JSON appid={} bytes={}", appId, r.body.size());
        return false;
    }

    LOG_NETPACKET_DEBUG("Example HTTP resolved appid={} value={} text={}", appId, out->value, out->text);
    return true;
}

} // namespace ExampleClient

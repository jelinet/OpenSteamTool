#include "ExampleClient.h"

#include "OSTPlatform/include/Http.h"
#include "Utils/Logging/Log.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string_view>

namespace ExampleClient {
namespace {

    // TODO: request URL. %u is replaced with the app id.
    constexpr const char* kUrlTemplate = "https://example.com/api/example/%u";

    // TODO: map your JSON response onto Result. Expected shape here:
    //   { "value": 123456, "text": "hello" }
    // parse(..., nullptr, false) never throws; malformed JSON is reported
    // via is_discarded(). Check types before get<>() — it throws on mismatch.
    bool ParseResponse(std::string_view body, Result* out) {
        const auto j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;

        const auto value = j.find("value");
        if (value == j.end() || !value->is_number_unsigned()) return false;
        out->value = value->get<uint64_t>();

        if (const auto text = j.find("text"); text != j.end() && text->is_string())
            out->text = text->get<std::string>();

        return true;
    }

} // namespace

bool Fetch(AppId_t appId, Result* out) {
    if (!out) return false;

    char url[256];
    std::snprintf(url, sizeof(url), kUrlTemplate, appId);

    // GET. For POST with a JSON body:
    //   const std::string req = nlohmann::json{{"app_id", appId}}.dump();
    //   auto r = OSTPlatform::Http::Execute(L"POST", url, req.data(),
    //       static_cast<uint32_t>(req.size()), L"Content-Type: application/json\r\n");
    auto r = OSTPlatform::Http::Execute(L"GET", url, nullptr, 0, L"Accept: application/json\r\n");
    LOG_NETPACKET_INFO("Example HTTP status={} appid={}", r.status, appId);

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

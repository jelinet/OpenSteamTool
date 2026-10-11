#pragma once

#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────────────
//  CDKeyReporter —— 把游戏 CDKey 的激活结果上报到 HTTP 接口。
//  由 Hooks_NetPacket_CDKey 在收到激活响应后调用，只读不改包。
//  需要填写的地方见 CDKeyReporter.cpp 里的 TODO（上报地址 + JSON 格式）。
// ─────────────────────────────────────────────────────────────────
namespace CDKeyReporter {

    struct Report {
        std::string key;            // 激活时输入的 CDKey
        uint64_t    steamId = 0;    // 执行激活的账号 SteamID（拿不到时为 0）
        int32_t     eresult = 0;    // EResult，1 = OK
        int32_t     detail  = 0;    // EPurchaseResultDetail，失败原因
    };

    // 把结果归类成一个简短的状态字符串，例如 "activated"、"used_by_other_account"。
    const char* Status(const Report& r);

    // 在后台线程发送，不阻塞调用方（Steam 网络线程）。
    void ReportAsync(Report r);

    // 日志用：只显示 key 的最后 5 位。
    std::string MaskKey(const std::string& key);

} // namespace CDKeyReporter

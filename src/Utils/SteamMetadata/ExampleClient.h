#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────────────
//  ExampleClient —— HTTP + JSON 请求模板，供 Hooks_NetPacket_Example 使用。
//  做真实功能时可以复制并改名，然后填写 ExampleClient.cpp 里的 TODO
//  （请求地址 + JSON 字段）。
//
//  Fetch() 会阻塞等待网络，不要在 Steam 网络线程的 hook 里直接调用，
//  要像 Hooks_NetPacket_Example 那样用 std::async 放到后台线程执行。
// ─────────────────────────────────────────────────────────────────
namespace ExampleClient {

    // TODO: 改成你想从 JSON 响应里取出的字段。
    struct Result {
        uint64_t    value = 0;
        std::string text;
    };

    // 成功返回 true 并填充 *out；HTTP 失败或 JSON 不符合预期返回 false。
    bool Fetch(AppId_t appId, Result* out);

} // namespace ExampleClient

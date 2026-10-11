#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────────────
//  ExampleClient — template HTTP + JSON client used by
//  Hooks_NetPacket_Example. Copy/rename it for a real feature and
//  fill in the TODOs in ExampleClient.cpp (URL + JSON fields).
//
//  Fetch() blocks on the network, so never call it directly from a
//  hook running on Steam's network thread — launch it with
//  std::async like Hooks_NetPacket_Example does.
// ─────────────────────────────────────────────────────────────────
namespace ExampleClient {

    // TODO: fields you want out of the JSON response.
    struct Result {
        uint64_t    value = 0;
        std::string text;
    };

    bool Fetch(AppId_t appId, Result* out);

} // namespace ExampleClient

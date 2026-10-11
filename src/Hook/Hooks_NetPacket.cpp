#include "Hooks_NetPacket.h"
#include "Utils/SteamMetadata/ManifestClient.h"
#include "Utils/SteamMetadata/CDKeyReporter.h"
#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Support/FnvHash.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "steam_messages.pb.h"

// ════════════════════════════════════════════════════════════════
//  Shared infrastructure
// ════════════════════════════════════════════════════════════════
namespace {

    constexpr uint32 kMaxBodySize   = 65536;
    constexpr uint32 kMaxHdrSize    = 1024;
    constexpr uint32 kMaxPacketSize = 8 + kMaxHdrSize + kMaxBodySize;
    constexpr int    kPacketPoolSize = 8;

    // ── Incoming (RecvPkt) packet pool ─────────────────────
    uint8  g_NewBody[kMaxBodySize];
    uint32 g_cbNewBody   = 0;
    uint8  g_NewHdr[kMaxHdrSize];
    uint32 g_cbNewHdr    = 0;
    bool   g_NeedReplaceBody = false;
    bool   g_NeedReplaceHdr  = false;
    bool   g_ResizedInPlace = false;
    uint32 g_NewBodySize    = 0;
    uint8  g_RecvPacketPool[kPacketPoolSize][kMaxPacketSize];
    int    g_RecvPacketPoolIdx = 0;

    // ── Outgoing (BBuildAndAsyncSendFrame) — same pattern ───────
    uint8  g_SendNewBody[kMaxBodySize];
    uint32 g_cbSendNewBody = 0;
    bool   g_NeedReplaceSend = false;
    bool   g_SuppressSend    = false;   // drop the outbound frame entirely (cloud RPC answered locally)
    uint8  g_SendPacketPool[kPacketPoolSize][kMaxPacketSize];
    int    g_SendPacketPoolIdx = 0;

    // ── EMsg -> name lookup  ─────────────────────────
    RESOLVE_FUNC(PchMsgNameFromEMsg, char*, EMsg eMsg);
    inline const char* MsgName(EMsg eMsg) {
        if (oPchMsgNameFromEMsg) return oPchMsgNameFromEMsg(eMsg);
        return "?";
    }


    // ── Packet layout ──────────────────────────────────────────
    inline bool UnpackRaw(const uint8* data, uint32 size,
                          EMsg& eMsg, const uint8*& pHdr, uint32& cbHdr,
                          const uint8*& pBody, uint32& cbBody)
    {
        if (!data || size < sizeof(MsgHdr)) {
        fail:
            eMsg = static_cast<EMsg>(0);
            cbHdr = 0;
            pHdr = nullptr;
            pBody = nullptr;
            cbBody = 0;
            return false;
        }
        const MsgHdr* hdr = reinterpret_cast<const MsgHdr*>(data);
        if (!(hdr->eMsg & kMsgHdrProtoFlag)) goto fail;

        eMsg  = static_cast<EMsg>(hdr->eMsg & ~kMsgHdrProtoFlag);
        cbHdr = hdr->headerLength;
        uint32 off = sizeof(MsgHdr) + cbHdr;
        if (off > size) goto fail;
        pHdr   = data + sizeof(MsgHdr);
        pBody  = data + off;
        cbBody = size - off;
        return true;
    }

    // ── Incoming: replace header and/or body (ring-buffer pool) ──
    // 把接收包替换成 [MsgHdr(eMsg + header 长度)] [新 header] [新 body]：
    // 在环形缓冲池 g_RecvPacketPool 里拼好新包，再把 p->m_pubData 指向它、
    // m_cubData 改成新长度。之后 RecvPkt 把这个包交给原函数 oRecvPkt，
    // Steam 看到的就是修改后的内容。新包超过缓冲大小时不替换，原包照常放行。
    // 用环形池而不是单个缓冲，是为了防止 Steam 还在用上一个包时被覆盖。
    inline void ReplaceRecvPacket(CNetPacket* p,
                                  const uint8* pNewHdr, uint32 cbNewHdr,
                                  const uint8* pNewBody, uint32 cbNewBody)
    {
        uint32 newSize = sizeof(MsgHdr) + cbNewHdr + cbNewBody;
        if (newSize > sizeof(g_RecvPacketPool[0])) return;

        uint8* buf = g_RecvPacketPool[g_RecvPacketPoolIdx];
        const MsgHdr* orig = reinterpret_cast<const MsgHdr*>(p->m_pubData);
        MsgHdr* out = reinterpret_cast<MsgHdr*>(buf);
        out->eMsg         = orig->eMsg;
        out->headerLength = cbNewHdr;
        memcpy(buf + sizeof(MsgHdr), pNewHdr, cbNewHdr);
        if (cbNewBody)
            memcpy(buf + sizeof(MsgHdr) + cbNewHdr, pNewBody, cbNewBody);
        p->m_pubData = buf;
        p->m_cubData = newSize;

        g_RecvPacketPoolIdx = (g_RecvPacketPoolIdx + 1) % kPacketPoolSize;
    }

    // ── Outgoing: assemble modified packet (ring-buffer pool) ────
    inline uint8* ReplaceSendPacket(const uint8* pubData,
                                    uint32 cbHdr, const uint8* pHdr,
                                    const uint8* pNewBody, uint32 cbNewBody,
                                    uint32* pNewSize)
    {
        *pNewSize = sizeof(MsgHdr) + cbHdr + cbNewBody;
        if (*pNewSize > sizeof(g_SendPacketPool[0])) return nullptr;

        uint8* buf = g_SendPacketPool[g_SendPacketPoolIdx];
        const MsgHdr* orig = reinterpret_cast<const MsgHdr*>(pubData);
        MsgHdr* out = reinterpret_cast<MsgHdr*>(buf);
        out->eMsg         = orig->eMsg;
        out->headerLength = cbHdr;
        memcpy(buf + sizeof(MsgHdr), pHdr, cbHdr);
        memcpy(buf + sizeof(MsgHdr) + cbHdr, pNewBody, cbNewBody);
        g_SendPacketPoolIdx = (g_SendPacketPoolIdx + 1) % kPacketPoolSize;
        return buf;
    }

    // ── Hash constants for target_job_name dispatch ─────────────
    // Service Method 名字的 FNV-1a 哈希，用在下面分发函数的 switch 里。
    // 新增要拦截的 service method 时，在这里加一个常量。
    constexpr uint32 HASH_JOB_NotifyRunningApps = Fnv1aHash("FamilyGroupsClient.NotifyRunningApps#1");      // 家庭共享：正在运行的游戏通知
    constexpr uint32 HASH_JOB_GetUserStats = Fnv1aHash("Player.GetUserStats#1");                            // 成就 / 统计数据
    constexpr uint32 HASH_JOB_GetManifestRequestCode = Fnv1aHash("ContentServerDirectory.GetManifestRequestCode#1");  // 清单下载码

} // anonymous namespace


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_AccessToken
//
//  Outgoing: CMsgClientPICSProductInfoRequest (eMsg 8903)
//
//  中文说明：发出 PICS 商品信息请求（8903）时，给 addappid 里配置了
//  access_token 的 app 填上 token，让 Steam 能拿到这些 app 的完整信息。
//  会改发送包。
//
//  背景：PICS（Product Info Cache Server）是 Steam 用来获取 app / package 元数据
//  （名字、depot 列表、清单 id、启动项等）的接口。部分受保护的游戏或 DLC，
//  请求里必须带正确的 access_token 才能拿到完整信息；没有的话服务器只返回
//  不完整的数据，Steam 就没法下载。这个 token 一般只有拥有者才会拿到。
//  token 来自 Lua 配置里的 addtoken(appid, "token")，例如：
//      addtoken(1361510, "2764735786934684318")
//
//  步骤（HandleSend）：
//    1. 解析发送包 body 为 CMsgClientPICSProductInfoRequest；
//    2. 先快速扫一遍：有没有 app 既在 addappid 里又配置了 token，
//       没有就返回 false，原包照常发送；
//    3. 再逐个 app 处理：在 addappid 里且有 token 的，把 access_token 改成配置的值；
//       在 addappid 里但没配置 token 的，跳过并写警告；不在 addappid 里的不动；
//    4. 把修改后的请求序列化到 g_SendNewBody，大小写进 g_cbSendNewBody；
//    5. 返回 true，SendJob 会设置 g_NeedReplaceSend，发送 hook 里的
//       ReplaceSendPacket 用 [MsgHdr] [原 header] [新 body] 重新组包发出。
//       header 不改，只换 body。
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_AccessToken {

    bool HandleSend(const uint8* pBody, uint32 cbBody)
    {
        // 步骤 1：解析请求。请求里 apps 是要查询的 app 列表，每项有 appid 和 access_token。
        CMsgClientPICSProductInfoRequest req;
        if (!req.ParseFromArray(pBody, cbBody)) {
            LOG_PICS_WARN("Failed to ParseFromArray CMsgClientPICSProductInfoRequest");
            return false;
        }
        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest original body:\n{}", req.DebugString());

        // 步骤 2：先只读地扫一遍，判断有没有需要注入 token 的 app。
        // 大部分 PICS 请求都不涉及 addappid 的游戏，这样可以避免无谓的重新序列化。
        bool needsPatch = false;
        for (const auto& app : req.apps()) {
            if (LuaConfig::HasDepot(app.appid()) && LuaConfig::GetAccessToken(app.appid())) {
                needsPatch = true;
                LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: found appid {} with access_token, need patching", app.appid());
                break;
            }
        }
        if (!needsPatch) {
            LOG_PICS_TRACE("CMsgClientPICSProductInfoRequest: no apps need token injection, skip");
            return false;
        }

        // 步骤 3：逐个 app 注入 token（mutable_apps 拿可修改的列表）。
        // 三个计数只用来写日志：已注入 / 在 addappid 里但没配 token / 不在 addappid 里。
        int injected = 0, noToken = 0, notAddAppId = 0;
        for (auto& app : *req.mutable_apps()) {
            if (LuaConfig::HasDepot(app.appid())) {
                uint64_t token = LuaConfig::GetAccessToken(app.appid());
                if (token) {
                    LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: inject appid={}: {} -> {}", app.appid(),
                               app.has_access_token() ? std::to_string(app.access_token()) : "absent",
                               token);
                    app.set_access_token(token);
                    ++injected;
                } else {
                    LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: skip appid={}: in depot, no token configured", app.appid());
                    ++noToken;
                }
            } else {
                ++notAddAppId;
            }
        }
        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: injected={} no_token={} not_in_add_appid={} total={}",
                   injected, noToken, notAddAppId, req.apps_size());

        // 步骤 4：序列化到发送替换缓冲 g_SendNewBody。超出缓冲大小或序列化失败时
        // 返回 false，不替换，原包照常发送（这时 token 没注入，但不会弄坏请求）。
        g_cbSendNewBody = static_cast<uint32>(req.ByteSizeLong());
        if (g_cbSendNewBody > kMaxBodySize) {
            LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: encoded size {} exceeds buffer", g_cbSendNewBody);
            return false;
        }
        if (!req.SerializeToArray(g_SendNewBody, kMaxBodySize)) {
            LOG_PICS_WARN("CMsgClientPICSProductInfoRequest: Failed to encode modified request");
            return false;
        }

        LOG_PICS_DEBUG("CMsgClientPICSProductInfoRequest: modified body: {}", req.DebugString());
        // 步骤 5：返回 true，表示发送包体需要换成 g_SendNewBody。
        return true;
    }

} // namespace Hooks_NetPacket_AccessToken


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_UserStats
//
//  Outgoing: CPlayer_GetUserStats_Request  (eMsg 151 -> target: Player.GetUserStats#1)
//            CMsgClientGetUserStats        (eMsg 818)
//  Incoming: CPlayer_GetUserStats_Response (eMsg 147 ← target: Player.GetUserStats#1)
//            CMsgClientGetUserStatsResponse(eMsg 819)
//
//  中文说明：成就 / 统计数据请求。
//  发送（151 Player.GetUserStats#1、818）：把查询的 SteamID 换成配置里的
//    统计账号（GetStatSteamId），818 还会强制重新下载 schema。
//  接收（147、819）：把 eresult 改成 OK，清掉借来的统计数据，
//    819 还会叠加 CloudRedirect 同步的成就。会改发送包和接收包。
//
//  背景：游戏的成就 / 统计分两部分：
//    - schema：这个游戏有哪些成就、哪些统计项（名字、图标等），所有人一样；
//    - stats：某个账号自己的进度和解锁时间。
//  对没拥有的游戏（addappid 加进来的），用自己的 SteamID 去查，服务器会返回失败，
//  Steam 拿不到 schema，成就功能就用不了。
//  做法：发送时把查询的 SteamID 换成一个拥有这个游戏的"统计账号"（下面叫借用账号），
//  让服务器正常返回 schema；接收时把 eresult 改成 OK，并把借用账号自己的
//  进度清掉，只留 schema，这样你看到的是自己的进度，而不是别人的。
//
//  借用账号的 SteamID 来自 LuaConfig::GetStatSteamId，优先级：
//    Lua 里的 setStat(appid, "steamid") > stats API（[stats] enable_api）> 内置默认 SteamID。
//
//  Steam 有新旧两套接口，所以有两对处理函数：
//    新：151 Player.GetUserStats#1  -> 147 响应（HandleSend_GetUserStats / HandleRecv_GetUserStatsResponse）
//    旧：818 ClientGetUserStats     -> 819 响应（HandleSend_ClientGetUserStats / HandleRecv_ClientGetUserStatsResponse）
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_UserStats {

    // jobid_source -> appid mapping (eMsg 151 request -> eMsg 147 response)
    // 147 响应的 body 里不一定带 appid，所以发送时按 jobid 记下 appid，
    // 收到响应时用 jobid_target 查回来，判断是不是 addappid 里的游戏。
    // 只在 Steam 网络线程里访问，所以没有加锁。
    std::unordered_map<uint64, AppId_t> g_JobIdToAppId;

    // ── Send: CPlayer_GetUserStats_Request (eMsg 151) ──────────
    // 新接口的请求。步骤：
    //   1. 解析请求，没有 appid 就不处理；
    //   2. 请求带了 sha_schema 时不改（原代码的判断，日志写的是 "do not spoof"）；
    //      不在 addappid 里的游戏也不改；
    //   3. 记下 jobid_source -> appid，给 147 响应用；
    //   4. 把 steamid 换成借用账号；
    //   5. 序列化到 g_SendNewBody，返回 true 让发送 hook 替换包体。
    bool HandleSend_GetUserStats(const uint8* pBody, uint32 cbBody,
                                 const uint8* pHdr, uint32 cbHdr)
    {

        // 步骤 1：解析请求
        CPlayer_GetUserStats_Request req;
        if (!req.ParseFromArray(pBody, cbBody)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: failed to ParseFromArray");
            return false;
        }
        if (!req.has_appid()) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: missing appid");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: original body:\n{}", req.DebugString());
        
        // 步骤 2：判断要不要改
        AppId_t appId = req.appid();
        bool hasShaSchema = req.has_sha_schema() && !req.sha_schema().empty();

        if (hasShaSchema) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: sha_schema is present, do not spoof");
            return false;
        }
        if (!LuaConfig::HasDepot(appId)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: appid={} is not in addappid", appId);
            return false;
        }

        // Save jobid_source -> appid for the response handler
        // 步骤 3：记下 jobid -> appid
        CMsgProtoBufHeader hdr;
        if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_jobid_source()) {
            uint64 jobId = hdr.jobid_source();
            g_JobIdToAppId[jobId] = appId;
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: stored jobid={} -> appid={}", jobId, appId);
        }

        // 步骤 4：换成借用账号的 SteamID
        uint64_t newSteamId = LuaConfig::GetStatSteamId(appId);
        req.set_steamid(newSteamId);

        // 步骤 5：序列化到发送替换缓冲

        g_cbSendNewBody = static_cast<uint32>(req.ByteSizeLong());
        if (!req.SerializeToArray(g_SendNewBody, kMaxBodySize)) {
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats request: failed to encode");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats request: modified body:\n{}", req.DebugString());
        return true;
    }

    // ── Recv: CPlayer_GetUserStats_Response (eMsg 147) ─────────
    //     Header: set eresult=OK.  Body: strip stats (field 4).
    // 新接口的响应。步骤：
    //   1. 解析 header，用 jobid_target 查回发送时记下的 appid（查到后删掉这条记录）；
    //   2. header：eresult 改成 OK，写到 g_NewHdr，设置 g_NeedReplaceHdr；
    //   3. body：只对查到 appid 且在 addappid 里的游戏，清掉 stats（借用账号的进度），
    //      保留 schema。
    //      清掉字段后 body 只会变小，所以直接写回原来的 body 缓冲（就地缩小），
    //      新长度放 g_NewBodySize，设置 g_ResizedInPlace；
    //      RecvPkt 看到它后会调整包长度（header 也换了时用 ReplaceRecvPacket 重新拼包）。
    // 注意：header 的 eresult 不管 appid 有没有查到都会改成 OK。
    void HandleRecv_GetUserStatsResponse(const uint8* pHdr, uint32 cbHdr,
                                    const uint8* pBody, uint32 cbBody)
    {
        // Header: set eresult=OK
        CMsgProtoBufHeader hdrMsg;
        if (!hdrMsg.ParseFromArray(pHdr, cbHdr)){
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats response: failed to ParseFromArray original header");
            return;
        }
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: original header:\n{}", hdrMsg.DebugString());

        // Look up appid via jobid_target -> jobid_source match
        // 步骤 1：按 jobid 查回 appid
        AppId_t appId = 0;
        bool hasAppId = false;
        if (hdrMsg.has_jobid_target()) {
            uint64 jobId = hdrMsg.jobid_target();
            auto it = g_JobIdToAppId.find(jobId);
            if (it != g_JobIdToAppId.end()) {
                appId = it->second;
                hasAppId = true;
                LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: matched jobid={} -> appid={}", jobId, appId);
                g_JobIdToAppId.erase(it);
            }
        }

        // 步骤 2：header 的 eresult 改成 OK
        hdrMsg.set_eresult(static_cast<int32_t>(k_EResultOK));
        g_cbNewHdr = static_cast<uint32>(hdrMsg.ByteSizeLong());
        if (g_cbNewHdr > kMaxHdrSize || !hdrMsg.SerializeToArray(g_NewHdr, kMaxHdrSize))
            return;
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: modified header:\n{}", hdrMsg.DebugString());
        g_NeedReplaceHdr = true;

        // Body: strip stats (only if appid was matched and is in our config)
        // 步骤 3：清掉借用账号的进度，就地写回 body
        CPlayer_GetUserStats_Response resp;
        if (!resp.ParseFromArray(pBody, cbBody)){
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats response: failed to ParseFromArray original response");
            return;
        }
        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: original body:\n{}", resp.DebugString());

        if (!hasAppId || !LuaConfig::HasDepot(appId)) {
            LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: no appid match, skip body strip");
            return;
        }

        resp.clear_stats();
        // 写回原 body 缓冲（const_cast：缓冲属于 Steam 的接收包，变小不会越界）
        g_NewBodySize = static_cast<uint32>(resp.ByteSizeLong());
        if (!resp.SerializeToArray(const_cast<uint8*>(pBody), cbBody)){
            LOG_ACHIEVEMENT_WARN("Player::GetUserStats response: failed to SerializeToArray modified response");
            return;
        }
        g_ResizedInPlace = true;

        LOG_ACHIEVEMENT_DEBUG("Player::GetUserStats response: modified body:\n{}", resp.DebugString());
    }

    // ── Send: CMsgClientGetUserStats (eMsg 818) ────────────────
    // 旧接口的请求。步骤：
    //   1. 解析请求，game_id 的低 32 位就是 appid，不在 addappid 里就不改；
    //   2. schema_local_version 设成 -1，告诉服务器"本地没有 schema"，
    //      强制它把完整 schema 发回来；
    //   3. steam_id_for_user 换成借用账号；
    //   4. 序列化到 g_SendNewBody，返回 true 让发送 hook 替换包体。
    // 旧接口的响应里自带 game_id，所以不需要像 151 那样记 jobid。
    bool HandleSend_ClientGetUserStats(const uint8* pBody, uint32 cbBody)
    {
        CMsgClientGetUserStats req;
        if (!req.ParseFromArray(pBody, cbBody)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: failed to ParseFromArray");
            return false;
        }
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: original body:\n{}", req.DebugString());

        if (!req.has_game_id()) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: missing game_id");
            return false;
        }
        AppId_t appId = static_cast<AppId_t>(req.game_id());
        if (!LuaConfig::HasDepot(appId)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: appid={} is not in addappid", appId);
            return false;
        }
        // 步骤 2：强制重新下载 schema
        if (req.schema_local_version() != -1) {
            req.set_schema_local_version(-1);
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: forced schema_local_version to -1");
        }

        // 步骤 3：换成借用账号的 SteamID
        uint64_t newSteamId = LuaConfig::GetStatSteamId(appId);
        req.set_steam_id_for_user(newSteamId);

        // 步骤 4：序列化到发送替换缓冲

        g_cbSendNewBody = static_cast<uint32>(req.ByteSizeLong());
        if (!req.SerializeToArray(g_SendNewBody, kMaxBodySize)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats request: failed to SerializeToArray");
            return false;
        }

        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats request: modified body:\n{}", req.DebugString());
        return true;
    }

    // ── Recv: CMsgClientGetUserStatsResponse (eMsg 819) ────────
    //     Clear donor stats, overlay CR achievements, patch eresult->OK.
    // 旧接口的响应。步骤：
    //   1. 解析 body，按 game_id 判断是不是 addappid 里的游戏，不是就不改；
    //   2. 清掉借用账号的 stats 和 achievement_blocks（进度和解锁时间），
    //      eresult 改成 OK（旧接口的 eresult 在 body 里，不在 header 里）；
    //   3. 从 CloudRedirect 取你自己云同步的成就（最多 64 块），每块写成
    //      一个 stat（32 位的解锁标志位）和一个 achievement_block（32 个解锁时间），
    //      并按同样方式算出 crc_stats；没有数据就保持清空，相当于全部未解锁；
    //   4. 序列化到 g_NewBody，设置 g_NeedReplaceBody，替换整个 body。
    bool HandleRecv_ClientGetUserStatsResponse(const uint8* pBody, uint32 cbBody)
    {
        CMsgClientGetUserStatsResponse resp;
        if (!resp.ParseFromArray(pBody, cbBody))
            return false;
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: original body:\n{}", resp.DebugString());
        if(!resp.has_game_id() || !LuaConfig::HasDepot(static_cast<AppId_t>(resp.game_id()))) {
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: no modification needed");
            return false;
        }
        // 步骤 2：清掉借用账号的进度，eresult 改 OK
        resp.clear_stats();
        resp.clear_achievement_blocks();
        resp.set_eresult(1);  // k_EResultOK

        // Overlay CR's cloud-synced achievement state
        // 步骤 3：叠加 CloudRedirect 里你自己的成就
        const auto appId = static_cast<uint32_t>(resp.game_id());
        CloudRedirectHost::AchievementBlock blocks[64];
        uint32_t n = CloudRedirectHost::GetAchievements(appId, blocks, 64);
        if (n > 0) {
            uint32_t crc = 0;
            for (uint32_t i = 0; i < n; i++) {
                auto* s = resp.add_stats();
                s->set_stat_id(blocks[i].statId);
                s->set_stat_value(blocks[i].bits);
                auto* ab = resp.add_achievement_blocks();
                ab->set_achievement_id(blocks[i].statId);
                for (uint32_t bit = 0; bit < 32; bit++)
                    ab->add_unlock_time(blocks[i].unlockTimes[bit]);
                crc ^= blocks[i].bits ^ blocks[i].statId;
            }
            resp.set_crc_stats(crc);
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: injected {} CR achievement blocks for app {}", n, appId);
        } else {
            LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: cleared stats/achievements (no CR data for app {})", appId);
        }

        // 步骤 4：序列化到接收替换缓冲
        auto newSize = resp.ByteSizeLong();
        if (newSize > sizeof(g_NewBody)) {
            LOG_ACHIEVEMENT_WARN("ClientGetUserStats response: modified message too large ({} bytes)", newSize);
            return false;
        }
        if (!resp.SerializeToArray(g_NewBody, sizeof(g_NewBody)))
            return false;

        g_cbNewBody = static_cast<uint32>(newSize);
        g_NeedReplaceBody = true;
        LOG_ACHIEVEMENT_DEBUG("ClientGetUserStats response: modified body:\n{}", resp.DebugString());
        return true;
    }

} // namespace Hooks_NetPacket_UserStats


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_ETicket
//
//  Incoming: CMsgClientRequestEncryptedAppTicketResponse (eMsg 5527)
//
//  中文说明：加密应用票据响应（5527）。Steam 返回失败时，从本地凭据
//  库取票据填进去并把 eresult 改成 OK。
//  注意：分发里这个 case 已注释掉，现在由 IPC 层
//  Hooks_IPC_ISteamUser::GetEncryptedAppTicketResponse 处理。
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_ETicket {

    void HandleEncryptedAppTicketResponse(const uint8* pBody, uint32 cbBody)
    {
        CMsgClientRequestEncryptedAppTicketResponse resp;
        if (!resp.ParseFromArray(pBody, cbBody)) {
            LOG_NETPACKET_WARN("ClientRequestEncryptedAppTicketResponse: failed to ParseFromArray");
            return;
        }
        LOG_NETPACKET_DEBUG("ClientRequestEncryptedAppTicketResponse: original body:\n{}", resp.DebugString());

        if (resp.eresult() == k_EResultOK) return;
        if (!LuaConfig::HasDepot(resp.app_id())) return;

        auto ticket = AppTicket::GetEncryptedTicketFromCredentialStore(resp.app_id());
        if (ticket.empty()) return;

        if (!resp.mutable_encrypted_app_ticket()->ParseFromArray(
                ticket.data(), static_cast<int>(ticket.size()))) {
            LOG_NETPACKET_WARN("ClientRequestEncryptedAppTicketResponse: failed to ParseFromArray EncryptedAppTicket");
            return;
        }

        resp.set_eresult(k_EResultOK);

        auto encSize = resp.ByteSizeLong();
        if (encSize > sizeof(g_NewBody)) {
            LOG_NETPACKET_WARN("ClientRequestEncryptedAppTicketResponse: modified message too large");
            return;
        }
        if (!resp.SerializeToArray(g_NewBody, sizeof(g_NewBody))) {
            LOG_NETPACKET_WARN("ClientRequestEncryptedAppTicketResponse: failed to SerializeToArray modified response");
            return;
        }
        
        LOG_NETPACKET_DEBUG("ClientRequestEncryptedAppTicketResponse: modified body:\n{}", resp.DebugString());

        g_cbNewBody = static_cast<uint32>(encSize);
        g_NeedReplaceBody = true;
    }

} // namespace Hooks_NetPacket_ETicket


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_FamilySharing
//
//  中文说明：家庭共享相关消息（147 FamilyGroupsClient.NotifyRunningApps#1、
//  9406 ClientSharedLibraryStopPlaying）。把消息体清空，
//  避免 Steam 因为家庭共享冲突把正在玩的游戏踢掉。会改接收包。
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_FamilySharing {

    void ClearBody(const uint8*, uint32)
    {
        LOG_NETPACKET_DEBUG("Clearing family sharing message...");
        g_cbNewBody = 0;
        g_NeedReplaceBody = true;
    }

} // namespace Hooks_NetPacket_FamilySharing


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_Manifest
//
//  Outgoing: ContentServerDirectory.GetManifestRequestCode#1  (eMsg 151)
//  Incoming: ContentServerDirectory.GetManifestRequestCode#1  (eMsg 147)
//
//  Launches an async HTTP fetch on send; the recv handler waits up to
//  12 s for the result and patches both header (eresult=OK) and body
//  (manifest_request_code).  On timeout or failure the original
//  response passes through unmodified.
//
//  中文说明：获取清单下载码（manifest request code）。
//  发送（151）：对 addappid 里的 depot，异步去 ManifestClient 的 HTTP 源取码；
//  接收（147）：最多等 12 秒，拿到就把 eresult 改成 OK 并填入下载码，
//  超时或失败就原样放行。会改接收包。
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_Manifest {

    // 背景：Steam 下载游戏前要先向服务器要一个"清单下载码"。
    // 账号不拥有这个游戏时，Valve 会返回失败（eresult 不是 OK，也没有下载码）。

    // jobid_source -> 后台取码任务的 future。发送时存进来，收到响应时按 jobid_target 取出。
    std::unordered_map<uint64, std::shared_future<uint64>> g_CodeFutures;
    std::mutex g_CodeMutex;
    // 收到响应时最多等后台 HTTP 多少秒。这段时间 Steam 网络线程会卡住。
    constexpr uint32 kMaxWaitSeconds = 12;

    // 第 1 步（发送，eMsg 151）：只处理 addappid 里配置了的 depot。
    // 用 std::async 开后台线程，调用 ManifestClient::FetchManifestRequestCode
    // 从第三方 HTTP 源取下载码，并把 future 按 jobid_source 存进 g_CodeFutures。
    // future 被存了起来，不会马上析构，所以这里不会阻塞。
    // 发送包本身不改，照常发给 Valve。

    bool HandleSend(const uint8* pBody, uint32 cbBody,
                    const uint8* pHdr, uint32 cbHdr)
    {
        CContentServerDirectory_GetManifestRequestCode_Request req;
        if (!req.ParseFromArray(pBody, cbBody)) {
            LOG_MANIFEST_WARN("GetManifestRequestCode: failed to parse request");
            return false;
        }
        if (!req.has_depot_id() || !req.has_manifest_id()) return false;
        if (!LuaConfig::HasDepot(req.depot_id())) return false;

        CMsgProtoBufHeader hdr;
        if (!hdr.ParseFromArray(pHdr, cbHdr) || !hdr.has_jobid_source()) {
            LOG_MANIFEST_WARN("GetManifestRequestCode: missing jobid_source in header");
            return false;
        }

        uint64 jobId       = hdr.jobid_source();
        uint64 manifestGid = req.manifest_id();
        uint32 depotId     = req.depot_id();
        uint32 appId       = req.has_app_id() ? req.app_id() : 0;

        LOG_MANIFEST_DEBUG("GetManifestRequestCode send: depot={} gid={} jobid={} app_id={}",
                            depotId, manifestGid, jobId, appId);

        auto task = std::async(std::launch::async,
            [manifestGid, depotId, appId]() -> uint64 {
                uint64 code = 0;
                ManifestClient::FetchManifestRequestCode(manifestGid, &code, appId, depotId);
                return code;
            });

        {
            std::lock_guard<std::mutex> lock(g_CodeMutex);
            g_CodeFutures[jobId] = task.share();
        }

        return false; // Don't modify the outgoing request body
    }

    // 第 2 步（接收，eMsg 147）：用响应 header 的 jobid_target 找到发送时存的 future，
    // 最多等 kMaxWaitSeconds 秒。超时或拿到的码为 0 就直接 return，原包放行，
    // Steam 看到的仍是 Valve 的原始结果。
    // 第 3 步：拿到码后替换 header 和 body（见下面的注释），
    // 真正写回包的是 RecvPkt hook 里的 ReplaceRecvPacket。
    // 结果：Steam 以为 Valve 返回了成功和有效的下载码，会用它去 CDN 下载清单。
    // 一般发送时 HTTP 就开始了，响应回来时多半已经完成，实际等待时间很短。
    void HandleRecv(const uint8* pBody, uint32 cbBody,
                    const uint8* pHdr, uint32 cbHdr)
    {
        CMsgProtoBufHeader hdr;
        if (!hdr.ParseFromArray(pHdr, cbHdr)){
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: failed to ParseFromArray original header");
            return;
        }

        uint64 jobId = hdr.jobid_target();
        std::shared_future<uint64> future;

        {
            std::lock_guard<std::mutex> lock(g_CodeMutex);
            auto it = g_CodeFutures.find(jobId);
            if (it == g_CodeFutures.end()) return;
            future = it->second;
            g_CodeFutures.erase(it); // Always clean up immediately
        }
        // Wait up to kMaxWaitSeconds seconds for the HTTP fetch to complete
        auto status = future.wait_for(std::chrono::seconds(kMaxWaitSeconds));
        if (status != std::future_status::ready) {
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: HTTP timed out for jobid={}", jobId);
            return;
        }

        uint64 code = future.get();
        if (!code) {
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: HTTP returned 0 for jobid={}", jobId);
            return;
        }

        LOG_MANIFEST_DEBUG("GetManifestRequestCode recv: injecting code={} for jobid={}",
                            code, jobId);

        // Header: set eresult=OK
        // header：在原 header 上只把 eresult 改成 OK，jobid 等其它字段不变，
        // 写到 g_NewHdr，并设置 g_NeedReplaceHdr。
        // 注意：header 先标记替换，如果下面 body 序列化失败，会出现 header 为 OK、
        // body 还是原始内容的情况（body 只有一个字段，实际几乎不会发生）。
        hdr.set_eresult(static_cast<int32_t>(k_EResultOK));
        g_cbNewHdr = static_cast<uint32>(hdr.ByteSizeLong());
        if (g_cbNewHdr > kMaxHdrSize || !hdr.SerializeToArray(g_NewHdr, kMaxHdrSize)){
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: failed to SerializeToArray modified header,"
                    "g_cbNewHdr: {}, kMaxHdrSize: {}", g_cbNewHdr, kMaxHdrSize);
            return;
        }
        g_NeedReplaceHdr = true;

        // Body: set manifest_request_code
        // body：不改原 body，新建一个响应，只填 manifest_request_code，
        // 写到 g_NewBody，并设置 g_NeedReplaceBody。
        CContentServerDirectory_GetManifestRequestCode_Response resp;
        resp.set_manifest_request_code(code);

        g_cbNewBody = static_cast<uint32>(resp.ByteSizeLong());
        if (g_cbNewBody > kMaxBodySize || !resp.SerializeToArray(g_NewBody, kMaxBodySize)){
            LOG_MANIFEST_WARN("GetManifestRequestCode recv: failed to SerializeToArray modified body,"
                "g_cbNewBody:{}, kMaxBodySize:{}", g_cbNewBody, kMaxBodySize);
            return;
        }
        g_NeedReplaceBody = true;
    }

} // namespace Hooks_NetPacket_Manifest


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_CDKey
//
//  发出：CMsgClientRegisterKey      (eMsg 743)  —— 激活游戏 CDKey
//  收到：CMsgClientPurchaseResponse (eMsg 763)  —— 激活结果
//
//  只读：发送时记下 key，收到响应后把 key + 结果交给 CDKeyReporter
//  同步上报。两个方向的包都不做任何修改。
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_CDKey {

    struct Pending {
        uint64      jobId;
        std::string key;
        uint64      steamId;
    };

    // 等待响应的激活请求（一般同时只有一个，限制长度防止堆积）。
    std::deque<Pending> g_Pending;
    std::mutex g_Mutex;
    constexpr size_t kMaxPending = 16;

    // Steam 的无效 job id（k_GIDNil），也是 proto 里的默认值。
    constexpr uint64 kInvalidJobId = ~0ull;

    inline bool IsValidJobId(uint64 id) { return id != 0 && id != kInvalidJobId; }

    void HandleSend(const uint8* pBody, uint32 cbBody,
                    const uint8* pHdr, uint32 cbHdr)
    {
        CMsgClientRegisterKey req;
        if (!req.ParseFromArray(pBody, cbBody) || !req.has_key()) {
            LOG_NETPACKET_WARN("CDKey send: failed to parse CMsgClientRegisterKey");
            return;
        }

        CMsgProtoBufHeader hdr;
        uint64 jobId = kInvalidJobId, steamId = 0;
        if (hdr.ParseFromArray(pHdr, cbHdr)) {
            jobId   = hdr.jobid_source();
            steamId = hdr.steamid();
        }

        LOG_NETPACKET_INFO("CDKey send: key={} jobid={}", CDKeyReporter::MaskKey(req.key()), jobId);

        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Pending.push_back({jobId, req.key(), steamId});
        while (g_Pending.size() > kMaxPending) g_Pending.pop_front();
    }

    void HandleRecv(const uint8* pBody, uint32 cbBody,
                    const uint8* pHdr, uint32 cbHdr)
    {
        CMsgClientPurchaseResponse resp;
        if (!resp.ParseFromArray(pBody, cbBody)) {
            LOG_NETPACKET_WARN("CDKey recv: failed to parse CMsgClientPurchaseResponse");
            return;
        }

        CMsgProtoBufHeader hdr;
        const uint64 jobId = hdr.ParseFromArray(pHdr, cbHdr) ? hdr.jobid_target() : kInvalidJobId;

        Pending pending;
        {
            std::lock_guard<std::mutex> lock(g_Mutex);
            auto it = g_Pending.end();
            if (IsValidJobId(jobId)) {
                // 优先用 jobid_target 精确匹配对应的请求。
                for (auto i = g_Pending.begin(); i != g_Pending.end(); ++i)
                    if (i->jobId == jobId) { it = i; break; }
            } else if (!g_Pending.empty()) {
                // 响应没带 job id 时，按发送顺序取最早的一个。
                it = g_Pending.begin();
            }
            if (it == g_Pending.end()) {
                // 763 也用于商店购买等其它场景，找不到对应的 key 就忽略。
                LOG_NETPACKET_DEBUG("CDKey recv: no pending key for jobid={}", jobId);
                return;
            }
            pending = std::move(*it);
            g_Pending.erase(it);
        }

        CDKeyReporter::Report report;
        report.key     = std::move(pending.key);
        report.steamId = pending.steamId;
        report.eresult = resp.eresult();
        report.detail  = resp.purchase_result_details();

        LOG_NETPACKET_INFO("CDKey recv: key={} eresult={} detail={} status={}",
                           CDKeyReporter::MaskKey(report.key), report.eresult,
                           report.detail, CDKeyReporter::Status(report));

        CDKeyReporter::SendReport(report);   // 同步上报，失败只写日志
    }

} // namespace Hooks_NetPacket_CDKey


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_RichPresence
//
//  Outgoing: CMsgClientGamesPlayed   (eMsg 742 / 5410)
//  Incoming: CMsgClientPersonaState  (eMsg 766)
//
//  The server drops friend-side broadcasts for unowned AppIds, so the
//  banner stays on the last cached state.  Cache real self-pushes and
//  re-deliver a patched copy through oRecvPkt by borrowing the next
//  carrier packet's data pointer.
//
//  中文说明：好友列表里的"正在玩"状态。
//  发送（742/5410 GamesPlayed、7501 RichPresenceUpload）：记下正在玩的
//    appid 和富状态键值；
//  接收（766 PersonaState）：缓存服务器推给自己的状态，并把正在玩的
//    游戏信息补进去，必要时借下一个收到的包注入一个伪造的 766，
//    让自己的客户端界面显示正确。会改接收包、注入包。
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_RichPresence {

    AppId_t g_PlayingAppId = 0;
    uint64  g_LocalSteamId = 0;

    // Most recent self-PersonaState bytes captured from a real server push.
    // Reused as the template every game launch.
    uint8   g_SelfHdr [kMaxHdrSize];
    uint32  g_cbSelfHdr      = 0;
    uint8   g_SelfBody[kMaxBodySize];
    uint32  g_cbSelfBody     = 0;
    bool    g_HaveSelfCached = false;

    // Manufactured PersonaState packet (eMsg 766) ready to inject.
    uint8   g_InjectPkt[kMaxPacketSize];
    uint32  g_cbInjectPkt   = 0;
    bool    g_InjectPending = false;

    // Rich presence KVs per AppId, captured from outbound
    // CMsgClientRichPresenceUpload.  Per-AppId so a multi-game stack
    // does not conflate KV state.
    std::unordered_map<AppId_t, std::vector<std::pair<std::string, std::string>>> g_RPKvsByAppId;

    // Walk Steam's binary KV1 stream (a top-level "RP" struct around
    // string KVs) and collect every string KV at any depth.  Type 0x00
    // starts a struct, 0x01 a string KV, 0x08 ends a struct.  String KVs
    // are null-terminated key + null-terminated value.
    static void ExtractStringKVs(const uint8* data, uint32 size,
                                 std::vector<std::pair<std::string, std::string>>& out)
    {
        uint32 pos = 0;
        int depth = 0;
        auto readCStr = [&](std::string& s) -> bool {
            uint32 start = pos;
            while (pos < size && data[pos] != 0) ++pos;
            if (pos >= size) return false;
            s.assign(reinterpret_cast<const char*>(data + start), pos - start);
            ++pos;
            return true;
        };
        while (pos < size) {
            uint8 type = data[pos++];
            if (type == 0x08) {
                if (depth > 0) { --depth; continue; }
                break;
            }
            if (type == 0x00) {
                std::string name;
                if (!readCStr(name)) return;
                ++depth;
            } else if (type == 0x01) {
                std::string key, value;
                if (!readCStr(key) || !readCStr(value)) return;
                out.emplace_back(std::move(key), std::move(value));
            } else {
                return;
            }
        }
    }

    // Patch the self Friend entry with the appid (0 = stopped) and per-app
    // KVs.  Mask status_flags's RichPresence bit (0x1000) on appid + empty
    // KVs so a freshly launched game's first inject does not wipe the UI's
    // m_mapRichPresence, which is rebuilt from rich_presence() whenever
    // that bit is set.
    static void ApplyGameFields(CMsgClientPersonaState& msg,
                                CMsgClientPersonaState::Friend* entry,
                                AppId_t appid)
    {
        // EClientPersonaStateFlag::k_EClientPersonaStateFlagRichPresence
        constexpr uint32 kStatusFlagRichPresence = 0x1000;

        if (appid) {
            entry->set_game_played_app_id(appid);
            entry->set_gameid(static_cast<uint64>(appid));
            std::string name = Hooks_Misc::GetGameNameByAppID(appid);
            if (!name.empty()) entry->set_game_name(name);
            entry->clear_rich_presence();
            auto it = g_RPKvsByAppId.find(appid);
            const bool hasKvs = (it != g_RPKvsByAppId.end()) && !it->second.empty();
            if (hasKvs) {
                for (const auto& [k, v] : it->second) {
                    auto* kv = entry->add_rich_presence();
                    kv->set_key(k);
                    kv->set_value(v);
                }
                msg.set_status_flags(msg.status_flags() | kStatusFlagRichPresence);
            } else {
                msg.set_status_flags(msg.status_flags() & ~kStatusFlagRichPresence);
            }
        } else {
            entry->clear_game_played_app_id();
            entry->clear_gameid();
            entry->clear_game_name();
            entry->clear_rich_presence();
            msg.set_status_flags(msg.status_flags() | kStatusFlagRichPresence);
        }
    }

    static bool BuildInject(AppId_t appid)
    {
        if (!g_HaveSelfCached) return false;

        CMsgClientPersonaState msg;
        if (!msg.ParseFromArray(g_SelfBody, g_cbSelfBody)) return false;

        // Find our entry (a self-push always contains it).
        CMsgClientPersonaState::Friend* entry = nullptr;
        for (int i = 0; i < msg.friends_size(); ++i) {
            auto* f = msg.mutable_friends(i);
            if (f->has_friendid() && f->friendid() == g_LocalSteamId) {
                entry = f;
                break;
            }
        }
        if (!entry) return false;

        ApplyGameFields(msg, entry, appid);

        uint32 hdrSize  = g_cbSelfHdr;
        uint32 bodySize = static_cast<uint32>(msg.ByteSizeLong());
        uint32 total    = sizeof(MsgHdr) + hdrSize + bodySize;
        if (total > sizeof(g_InjectPkt) || bodySize > kMaxBodySize) {
            LOG_RICHPRESENCE_WARN("Inject packet too large ({} bytes)", total);
            return false;
        }

        auto* mhdr = reinterpret_cast<MsgHdr*>(g_InjectPkt);
        mhdr->eMsg = static_cast<EMsg>(
            static_cast<uint32>(k_EMsgClientPersonaState) | kMsgHdrProtoFlag);
        mhdr->headerLength = hdrSize;
        memcpy(g_InjectPkt + sizeof(MsgHdr), g_SelfHdr, hdrSize);
        if (!msg.SerializeToArray(g_InjectPkt + sizeof(MsgHdr) + hdrSize, bodySize))
            return false;

        g_cbInjectPkt = total;
        LOG_RICHPRESENCE_INFO("Built inject for appid {} ({} bytes)", appid, total);
        return true;
    }

    // Decode outbound CMsgClientRichPresenceUpload into per-AppId KVs
    // and stage a fresh PersonaState inject.
    void TrackRPSend(const uint8* pBody, uint32 cbBody)
    {
        if (g_LocalSteamId == 0 || g_PlayingAppId == 0) return;

        CMsgClientRichPresenceUpload up;
        if (!up.ParseFromArray(pBody, cbBody)) return;
        if (!up.has_rich_presence_kv()) return;

        const std::string& kv = up.rich_presence_kv();
        auto& kvs = g_RPKvsByAppId[g_PlayingAppId];
        kvs.clear();
        ExtractStringKVs(reinterpret_cast<const uint8*>(kv.data()),
                         static_cast<uint32>(kv.size()), kvs);
        LOG_RICHPRESENCE_DEBUG("RP upload appid={}: kv_bytes={} extracted={} pairs",
            g_PlayingAppId, kv.size(), kvs.size());

        if (BuildInject(g_PlayingAppId)) g_InjectPending = true;
    }

    void TrackSend(const CMsgClientGamesPlayed& msg, const uint8* pHdr, uint32 cbHdr)
    {
        if (g_LocalSteamId == 0) {
            CMsgProtoBufHeader hdr;
            if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_steamid() && hdr.steamid()) {
                g_LocalSteamId = hdr.steamid();
                LOG_RICHPRESENCE_DEBUG("Captured local SteamID 0x{:X}", g_LocalSteamId);
                CloudRedirectHost::SetAccountId(
                    static_cast<uint32_t>(g_LocalSteamId & 0xFFFFFFFF));
            }
        }

        // Steam stacks running games in games_played; the banner follows
        // the tail (most recently launched).  Mirror that — only the
        // tail's appid drives our inject.
        AppId_t topmost = 0;
        if (msg.games_played_size() > 0) {
            topmost = static_cast<AppId_t>(
                msg.games_played(msg.games_played_size() - 1).game_id() & UINT32_MAX);
        }

        // Only track when the topmost is an unlocked AppId we can inject for.
        // Owned games on top let the server's natural broadcast paint the
        // cache; -onlinefix games are already handled by the OnlineFix path.
        AppId_t newTracked = 0;
        if (topmost != 0 && topmost != kOnlineFixAppId && LuaConfig::HasDepot(topmost))
            newTracked = topmost;

        if (g_PlayingAppId == newTracked) return;
        AppId_t oldTracked = g_PlayingAppId;
        g_PlayingAppId = newTracked;

        if (oldTracked != 0)
            CloudRedirectHost::NotifyAppRunning(oldTracked, false);
        if (newTracked != 0)
            CloudRedirectHost::NotifyAppRunning(newTracked, true);

        if (newTracked != 0) {
            LOG_RICHPRESENCE_INFO("Tracking topmost appid {}", newTracked);
            if (BuildInject(newTracked)) g_InjectPending = true;
        } else if (topmost == 0) {
            // Stack went empty — inject a clear so the cache reverts.
            LOG_RICHPRESENCE_DEBUG("GamesPlayed empty, scheduling cache clear");
            if (BuildInject(0)) g_InjectPending = true;
        } else {
            // Topmost is owned (or -onlinefix); let the server's broadcast
            // paint it.  Skipping the clear-inject here avoids a brief
            // "Online" flicker between our drop and the server's push.
            LOG_RICHPRESENCE_DEBUG("Topmost is appid {} (owned or onlinefix); deferring to server", topmost);
        }
    }

    // Cache real self-pushes as the inject template; if we are tracking
    // an unowned game, patch the live message in place so a periodic
    // refresh does not overwrite the injected game info.
    bool HandleRecv(const uint8* pBody, uint32 cbBody, const uint8* pHdr, uint32 cbHdr)
    {
        CMsgClientPersonaState msg;
        if (!msg.ParseFromArray(pBody, cbBody)) return false;

        CMsgClientPersonaState::Friend* selfEntry = nullptr;
        for (int i = 0; i < msg.friends_size(); ++i) {
            auto* f = msg.mutable_friends(i);
            if (f->has_friendid() && f->friendid() == g_LocalSteamId) {
                selfEntry = f;
                break;
            }
        }
        if (!selfEntry) return false;

        LOG_RICHPRESENCE_DEBUG(
            "Recv self PersonaState: status_flags=0x{:X} friends_size={}",
            msg.status_flags(), msg.friends_size());

        if (cbHdr <= sizeof(g_SelfHdr) && cbBody <= sizeof(g_SelfBody)) {
            memcpy(g_SelfHdr,  pHdr,  cbHdr);
            memcpy(g_SelfBody, pBody, cbBody);
            g_cbSelfHdr      = cbHdr;
            g_cbSelfBody     = cbBody;
            g_HaveSelfCached = true;
        }

        if (g_PlayingAppId == 0) return false;

        ApplyGameFields(msg, selfEntry, g_PlayingAppId);
        g_cbNewBody = static_cast<uint32>(msg.ByteSizeLong());
        if (g_cbNewBody > kMaxBodySize) {
            LOG_RICHPRESENCE_WARN("In-place patch too large ({} bytes)", g_cbNewBody);
            return false;
        }
        if (!msg.SerializeToArray(g_NewBody, kMaxBodySize)) {
            LOG_RICHPRESENCE_WARN("In-place patch SerializeToArray failed");
            return false;
        }
        LOG_RICHPRESENCE_INFO("Patched live self push with appid {}", g_PlayingAppId);
        return true;
    }

    // Deliver the pending manufactured PersonaState by borrowing the
    // carrier's data pointer for one oRecvPkt call, then restore.
    void TryInject(void* pThis, CNetPacket* pCarrier,
                   bool (*invokeOriginal)(void*, CNetPacket*))
    {
        if (!g_InjectPending || g_cbInjectPkt == 0) return;
        g_InjectPending = false;

        uint8* origData = pCarrier->m_pubData;
        uint32 origSize = pCarrier->m_cubData;
        pCarrier->m_pubData = g_InjectPkt;
        pCarrier->m_cubData = g_cbInjectPkt;
        invokeOriginal(pThis, pCarrier);
        pCarrier->m_pubData = origData;
        pCarrier->m_cubData = origSize;
        LOG_RICHPRESENCE_INFO("Delivered manufactured self-PersonaState ({} bytes)", g_cbInjectPkt);
    }

} // namespace Hooks_NetPacket_RichPresence


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_OnlineFix
//
//  Outgoing: CMsgClientGamesPlayed (eMsg 742 / 5410)
//
//  When a game launched with -onlinefix reports appid 480, replace
//  game_extra_info with the real game's localized name so friends
//  see the correct title.
//
//  中文说明：上报正在玩的游戏（742/5410）。用 -onlinefix 启动的游戏
//  会以 appid 480 上报，这里把 game_extra_info 改成真实游戏名，
//  让好友看到正确的游戏名。会改发送包。
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_OnlineFix {

    bool HandleSend(const uint8* pBody, uint32 cbBody,
                    const uint8* pHdr, uint32 cbHdr)
    {
        CMsgClientGamesPlayed msg;
        if (!msg.ParseFromArray(pBody, cbBody)) {
            LOG_ONLINEFIX_WARN("OnlineFix: failed to parse CMsgClientGamesPlayed");
            return false;
        }
        LOG_ONLINEFIX_DEBUG("OnlineFix: original body:\n{}", msg.DebugString());

        Hooks_NetPacket_RichPresence::TrackSend(msg, pHdr, cbHdr);

        bool patched = false;
        for (int i = 0; i < msg.games_played_size(); ++i) {
            auto* game = msg.mutable_games_played(i);
            AppId_t appid = static_cast<AppId_t>(game->game_id() & UINT32_MAX);

            // SpawnProcess rewrites pGameID to 480, so game_id is already 480.
            // Fill game_extra_info with the real game name.
            if (appid == kOnlineFixAppId) {
                AppId_t realAppId = Hooks_Misc::ResolveAppId();
                if (realAppId && LuaConfig::HasDepot(realAppId)) {
                    std::string name = Hooks_Misc::GetGameNameByAppID(realAppId);
                    if (!name.empty()) {
                        game->set_game_extra_info(name);
                        patched = true;
                        LOG_ONLINEFIX_INFO("OnlineFix: 480 -> name '{}' (real appid {})",
                            name, realAppId);
                    }
                }
            }
        }

        if (!patched) return false;

        g_cbSendNewBody = static_cast<uint32>(msg.ByteSizeLong());
        if (g_cbSendNewBody > kMaxBodySize) {
            LOG_ONLINEFIX_WARN("OnlineFix: encoded size {} exceeds buffer", g_cbSendNewBody);
            return false;
        }
        if (!msg.SerializeToArray(g_SendNewBody, kMaxBodySize)) {
            LOG_ONLINEFIX_WARN("OnlineFix: failed to SerializeToArray");
            return false;
        }

        LOG_ONLINEFIX_DEBUG("OnlineFix: modified body:\n{}", msg.DebugString());
        return true;
    }

} // namespace Hooks_NetPacket_OnlineFix


// ════════════════════════════════════════════════════════════════
//  Hooks_NetPacket_Cloud
//
//  Steam Cloud save redirection via CloudRedirect (cloud_redirect.dll).
//
//  Outgoing: ServiceMethodCallFromClient (eMsg 151) with target_job_name
//            "Cloud.*" for an addappid()-unlocked game.
//  Incoming: a synthesized ServiceMethodResponse (eMsg 147) carrying the
//            answer produced by CloudRedirect, correlated by jobid.
//
//  CloudRedirect answers the RPC locally (reading/writing the real save
//  bytes to the user's cloud provider). We therefore SUPPRESS the outbound
//  request (it must not reach Valve) and DELIVER the response the same way
//  the RichPresence path injects packets: by borrowing the next inbound
//  "carrier" packet for one oRecvPkt call (CloudRedirect's "Approach D").
//
//  中文说明：Steam 云存档重定向（cloud_redirect.dll）。
//  发送（151 Cloud.*）：交给 CloudRedirect 在本地处理，处理了就
//    拦下这个包不发给 Valve（g_SuppressSend）；
//  接收：把 CloudRedirect 生成的 147 响应借下一个收到的包注入给 Steam。
// ════════════════════════════════════════════════════════════════
namespace Hooks_NetPacket_Cloud {

    std::mutex                     g_queueMutex;
    std::deque<std::vector<uint8>> g_pending;        // ready-to-inject 147 packets
    uint64                         g_localSteamId = 0;

    // ── minimal top-level protobuf varint field reader ──────────
    // Avoids pulling the Cloud.* request message definitions into the
    // proto set just to read one appid field.
    static bool ReadVarint(const uint8* d, uint32 size, uint32& pos, uint64& out) {
        out = 0;
        int shift = 0;
        while (pos < size) {
            uint8 b = d[pos++];
            out |= static_cast<uint64>(b & 0x7F) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
            if (shift >= 64) return false;
        }
        return false;
    }

    static bool FindVarintField(const uint8* d, uint32 size, uint32 target, uint64& out) {
        uint32 pos = 0;
        while (pos < size) {
            uint64 tag;
            if (!ReadVarint(d, size, pos, tag)) return false;
            const uint32 field   = static_cast<uint32>(tag >> 3);
            const uint32 wireType = static_cast<uint32>(tag & 7);
            if (field == target && wireType == 0)
                return ReadVarint(d, size, pos, out);

            switch (wireType) {
            case 0: { uint64 tmp; if (!ReadVarint(d, size, pos, tmp)) return false; break; }
            case 1: if (pos + 8 > size) return false; pos += 8; break;
            case 5: if (pos + 4 > size) return false; pos += 4; break;
            case 2: {
                uint64 len;
                if (!ReadVarint(d, size, pos, len)) return false;
                if (pos + len > size) return false;
                pos += static_cast<uint32>(len);
                break;
            }
            default: return false;   // groups (3/4) — bail
            }
        }
        return false;
    }

    // appid lives in field 1 of every Cloud.* request except
    // ClientCommitFileUpload, where it is field 2 (mirrors CloudRedirect's
    // CloudRpcUtils::ExtractAppId).
    static uint32 ExtractAppId(const char* jobName, const uint8* body, uint32 cbBody) {
        uint32 fieldNum = 1;
        if (std::strcmp(jobName, "Cloud.ClientCommitFileUpload#1") == 0)
            fieldNum = 2;
        uint64 v = 0;
        if (FindVarintField(body, cbBody, fieldNum, v))
            return static_cast<uint32>(v);
        return 0;
    }

    // Returns true when CloudRedirect handled the request — the caller must
    // then suppress the outbound frame.
    bool HandleSend(const char* jobName,
                    const uint8* pBody, uint32 cbBody,
                    const uint8* pHdr, uint32 cbHdr)
    {
        if (!CloudRedirectHost::IsActive()) return false;

        CMsgProtoBufHeader reqHdr;
        if (!reqHdr.ParseFromArray(pHdr, cbHdr)) return false;
        if (reqHdr.has_steamid() && reqHdr.steamid())
            g_localSteamId = reqHdr.steamid();

        const uint32 appId = ExtractAppId(jobName, pBody, cbBody);
        if (appId == 0 || !CloudRedirectHost::IsApp(appId)) return false;

        const uint32 accountId = static_cast<uint32>(g_localSteamId & 0xFFFFFFFFull);

        static thread_local uint8 respBuf[kMaxBodySize];
        uint32  respLen  = 0;
        int32_t eresult  = 2;   // EResult::Fail
        if (!CloudRedirectHost::HandleCloudRpc(jobName, appId, accountId,
                                               pBody, cbBody,
                                               respBuf, static_cast<uint32_t>(sizeof(respBuf)),
                                               &respLen, &eresult)) {
            return false;   // not a namespace app / unrecognised — let it pass through
        }

        // Build the 147 ServiceMethodResponse correlated to the request job.
        CMsgProtoBufHeader respHdr;
        if (reqHdr.has_jobid_source()) respHdr.set_jobid_target(reqHdr.jobid_source());
        respHdr.set_eresult(eresult);
        respHdr.set_target_job_name(jobName);

        const uint32 cbRespHdr = static_cast<uint32>(respHdr.ByteSizeLong());
        const uint32 total     = sizeof(MsgHdr) + cbRespHdr + respLen;
        if (cbRespHdr > kMaxHdrSize || respLen > kMaxBodySize || total > kMaxPacketSize) {
            // Can't deliver a response this big — fall through so Steam errors
            // normally instead of leaving the job hung.
            LOG_NETPACKET_WARN("Cloud: {} response too large ({} bytes), passing through",
                               jobName, total);
            return false;
        }

        std::vector<uint8> pkt(total);
        auto* mhdr = reinterpret_cast<MsgHdr*>(pkt.data());
        mhdr->eMsg = static_cast<EMsg>(
            static_cast<uint32>(k_EMsgServiceMethodResponse) | kMsgHdrProtoFlag);
        mhdr->headerLength = cbRespHdr;
        if (!respHdr.SerializeToArray(pkt.data() + sizeof(MsgHdr), cbRespHdr))
            return false;
        if (respLen)
            memcpy(pkt.data() + sizeof(MsgHdr) + cbRespHdr, respBuf, respLen);

        {
            std::lock_guard lk(g_queueMutex);
            if (g_pending.size() < 64)
                g_pending.push_back(std::move(pkt));
        }
        LOG_NETPACKET_DEBUG("Cloud: handled {} app={} -> queued {}-byte response (eresult={})",
                            jobName, appId, total, eresult);
        return true;
    }

    // Deliver any queued cloud responses by borrowing the carrier packet for
    // one oRecvPkt call each (same trick as RichPresence::TryInject). Runs on
    // the network thread from inside the RecvPkt hook.
    void Drain(void* pThis, CNetPacket* pCarrier,
               bool (*invokeOriginal)(void*, CNetPacket*))
    {
        for (;;) {
            std::vector<uint8> pkt;
            {
                std::lock_guard lk(g_queueMutex);
                if (g_pending.empty()) return;
                pkt = std::move(g_pending.front());
                g_pending.pop_front();
            }

            uint8* origData = pCarrier->m_pubData;
            uint32 origSize = pCarrier->m_cubData;
            pCarrier->m_pubData = pkt.data();
            pCarrier->m_cubData = static_cast<uint32>(pkt.size());
            invokeOriginal(pThis, pCarrier);
            pCarrier->m_pubData = origData;
            pCarrier->m_cubData = origSize;
            LOG_NETPACKET_DEBUG("Cloud: delivered {}-byte response", pkt.size());
        }
    }

} // namespace Hooks_NetPacket_Cloud


// ════════════════════════════════════════════════════════════════
//  Dispatch
// ════════════════════════════════════════════════════════════════
namespace {

    // 发送方向的 Service Method（eMsg 151）分发：按 header 里的 target_job_name 选处理函数。
    // 返回 true 表示处理函数已经把新的包体写进 g_SendNewBody，需要替换发送包。
    bool SendServiceJob(const char* targetJobName,
                        const uint8* pBody, uint32 cbBody,
                        const uint8* pHdr, uint32 cbHdr)
    {
        LOG_NETPACKET_DEBUG("Send target_job_name: {}", targetJobName);

        // Steam Cloud save redirection: hand every "Cloud.*" request to
        // CloudRedirect. If it answers, suppress the outbound frame (the
        // synthesized response is delivered from the RecvPkt hook).
        // ExitSyncDone/ConflictResolution are notifications that must reach
        // Steam's internal cloud state machine untouched.
        if (std::strncmp(targetJobName, "Cloud.", 6) == 0) {
            if (std::strcmp(targetJobName, "Cloud.SignalAppExitSyncDone#1") == 0 ||
                std::strcmp(targetJobName, "Cloud.ClientConflictResolution#1") == 0)
                return false;
            if (Hooks_NetPacket_Cloud::HandleSend(targetJobName, pBody, cbBody, pHdr, cbHdr))
                g_SuppressSend = true;
            return false;   // never body-replace a cloud frame
        }

        switch (Fnv1aHash(targetJobName)) {

        // 查询成就 / 统计：把 SteamID 换成配置的统计账号
        case HASH_JOB_GetUserStats:
            return Hooks_NetPacket_UserStats::HandleSend_GetUserStats(pBody, cbBody, pHdr, cbHdr);

        // 请求清单下载码：异步去 HTTP 源取码，等响应回来时替换
        case HASH_JOB_GetManifestRequestCode:
            return Hooks_NetPacket_Manifest::HandleSend(pBody, cbBody, pHdr, cbHdr);

        // ---- add new 151 service methods here ----
        }
        return false;
    }

    // 发送方向的总分发：Steam 每发一个包都会经过这里，按 eMsg 选处理函数。
    // 处理函数可以设置 g_NeedReplaceSend（替换包体）或 g_SuppressSend（不发出去）。
    void SendJob(EMsg eMsg, const uint8* pBody, uint32 cbBody,
                 const uint8* pHdr, uint32 cbHdr)
    {
        g_NeedReplaceSend = false;
        g_SuppressSend    = false;

        LOG_NETPACKET_DEBUG("Send eMsg {}({}) (cbBody={}, cbHdr={})",
                        MsgName(eMsg), static_cast<uint32>(eMsg), cbBody, cbHdr);

        switch (eMsg) {

        case k_EMsgServiceMethodCallFromClient: {   // 151  Service Method 请求，交给 SendServiceJob
            CMsgProtoBufHeader hdr;
            if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_target_job_name()) {
                g_NeedReplaceSend = SendServiceJob(hdr.target_job_name().c_str(), pBody, cbBody, pHdr, cbHdr);
            }
            return;
        }

        case k_EMsgClientPICSProductInfoRequest:     // 8903 PICS 商品信息请求：注入 access_token
            g_NeedReplaceSend = Hooks_NetPacket_AccessToken::HandleSend(pBody, cbBody);
            return;

        case k_EMsgClientGamesPlayed:                 // 742  上报正在玩的游戏
        case k_EMsgClientGamesPlayedWithDataBlob:     // 5410 同上（带数据块）：OnlineFix 改游戏名 + 记录正在玩
            g_NeedReplaceSend = Hooks_NetPacket_OnlineFix::HandleSend(pBody, cbBody, pHdr, cbHdr);
            return;

        case k_EMsgClientRegisterKey:                 // 743  激活 CDKey（只记录，不改包）
            Hooks_NetPacket_CDKey::HandleSend(pBody, cbBody, pHdr, cbHdr);
            return;

        case k_EMsgClientRichPresenceUpload:           // 7501 上传富状态：记下键值，用于注入 766（不改包）
            Hooks_NetPacket_RichPresence::TrackRPSend(pBody, cbBody);
            return;

        case k_EMsgClientGetUserStats:               // 818  旧版成就 / 统计请求：换 SteamID
            g_NeedReplaceSend = Hooks_NetPacket_UserStats::HandleSend_ClientGetUserStats(pBody, cbBody);
            return;

        case k_EMsgClientStoreUserStats2: {         // 5466 保存成就 / 统计：通知 CloudRedirect（不改包）
            AppId_t appId = Hooks_NetPacket_RichPresence::g_PlayingAppId;
            if (appId != 0)
                CloudRedirectHost::NotifyStatsStored(appId);
            return;
        }

        default:
            return;
        }
    }

    // 接收方向的 Service Method 响应（eMsg 147）分发：按 header 里的 target_job_name 选处理函数。
    void RecvServiceJob(const char* targetJobName,
                        const uint8* pBody, uint32 cbBody,
                        const uint8* pHdr, uint32 cbHdr)
    {
        LOG_NETPACKET_DEBUG("Recv target_job_name: {}", targetJobName);
        g_NeedReplaceBody = false;
        g_NeedReplaceHdr  = false;

        switch (Fnv1aHash(targetJobName)) {

        // 家庭共享正在运行的游戏通知：清空消息体
        case HASH_JOB_NotifyRunningApps:
            Hooks_NetPacket_FamilySharing::ClearBody(pBody, cbBody);
            return;

        // 成就 / 统计响应：eresult 改成 OK，清掉借来的统计数据
        case HASH_JOB_GetUserStats:
            Hooks_NetPacket_UserStats::HandleRecv_GetUserStatsResponse(pHdr, cbHdr, pBody, cbBody);
            return;

        // 清单下载码响应：填入从 HTTP 源拿到的下载码
        case HASH_JOB_GetManifestRequestCode:
            Hooks_NetPacket_Manifest::HandleRecv(pBody, cbBody, pHdr, cbHdr);
            return;

        // ---- add new 147 service methods here ----
        }
    }

    // 接收方向的总分发：Steam 每收到一个包都会经过这里，按 eMsg 选处理函数。
    // 处理函数通过 g_NeedReplaceBody / g_NeedReplaceHdr 决定是否替换包体和 header。
    void RecvJob(EMsg eMsg, const uint8* pBody, uint32 cbBody,
                 const uint8* pHdr, uint32 cbHdr)
    {
        g_NeedReplaceBody = false;
        g_NeedReplaceHdr  = false;

        // k_EMsgMulti 是把多个包打包压缩在一起的容器，里面的包会单独再经过这里，这里直接跳过。
        if(eMsg == k_EMsgMulti) {
            LOG_NETPACKET_TRACE("Received k_EMsgMulti, skipping dispatch");
            return;
        }
        LOG_NETPACKET_DEBUG("Recv eMsg {}({}) (cbBody={}, cbHdr={})",
                        MsgName(eMsg), static_cast<uint32>(eMsg), cbBody, cbHdr);

        switch (eMsg) {

        case k_EMsgServiceMethodResponse: {     // 147  Service Method 响应，交给 RecvServiceJob
            CMsgProtoBufHeader hdr;
            if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_target_job_name())
                RecvServiceJob(hdr.target_job_name().c_str(), pBody, cbBody, pHdr, cbHdr);
            return;
        }

        // migrated to IPC Layer Hooks_IPC_ISteamUser::GetEncryptedAppTicketResponse
        // （已迁移到 IPC 层处理，这里不再拦截 5527）
        // case k_EMsgClientRequestEncryptedAppTicketResponse:     // 5527
        //     Hooks_NetPacket_ETicket::HandleEncryptedAppTicketResponse(pBody, cbBody);
        //     return;

        case k_EMsgClientGetUserStatsResponse:     // 819  旧版成就 / 统计响应：eresult 改 OK，叠加 CloudRedirect 成就
            g_NeedReplaceBody = Hooks_NetPacket_UserStats::HandleRecv_ClientGetUserStatsResponse(
                pBody, cbBody);
            return;

        case k_EMsgClientPurchaseResponse:             // 763  CDKey 激活结果（只记录，不改包）
            Hooks_NetPacket_CDKey::HandleRecv(pBody, cbBody, pHdr, cbHdr);
            return;

        case k_EMsgClientSharedLibraryStopPlaying:     // 9406 家庭共享"停止游戏"通知：清空消息体
            Hooks_NetPacket_FamilySharing::ClearBody(pBody, cbBody);
            return;

        case k_EMsgClientPersonaState:                 // 766  好友状态推送：补上正在玩的游戏信息
            g_NeedReplaceBody = Hooks_NetPacket_RichPresence::HandleRecv(pBody, cbBody, pHdr, cbHdr);
            return;

        default:
            return;
        }
    }

    // ════════════════════════════════════════════════════════════
    //  Hooks
    // ════════════════════════════════════════════════════════════

    // 发送 hook：Steam 通过 WebSocket 给 CM 服务器发每一帧之前都会调用。
    // 流程：拆包（eMsg + header + body）→ SendJob 分发 →
    //   g_SuppressSend  为 true：不发出去，直接返回成功（CloudRedirect 已在本地处理）；
    //   g_NeedReplaceSend 为 true：用 g_SendNewBody 重新组包再发；
    //   否则原样发送。
    // 非二进制帧（比如文本、ping）直接放行。
    HOOK_FUNC(BBuildAndAsyncSendFrame, bool,
              void* pObject, EWebSocketOpCode eWebSocketOpCode,
              uint8* pubData, uint32 cubData)
    {
        if (eWebSocketOpCode != k_eWebSocketOpCode_Binary)
            return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);

        EMsg eMsg;
        const uint8 *pHdr, *pBody;
        uint32 cbHdr, cbBody;
        bool result;
        if (UnpackRaw(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) {
            SendJob(eMsg, pBody, cbBody, pHdr, cbHdr);

            if (g_SuppressSend) {
                // CloudRedirect answered this RPC locally; do not forward to
                // Valve. Report success so Steam treats the frame as sent.
                return true;
            }

            if (g_NeedReplaceSend) {
                uint32 newSize = 0;
                uint8* buf = ReplaceSendPacket(pubData, cbHdr, pHdr,
                                               g_SendNewBody, g_cbSendNewBody, &newSize);
                result = buf
                    ? oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, buf, newSize)
                    : oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);
            } else {
                result = oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);
            }
        } else {
            result = oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);
        }

        return result;
    }

    // 接收 hook：Steam 每收到 CM 服务器的一个包都会调用，交给 Steam 处理之前先经过这里。
    // 流程：
    //   1. 先借这个包注入待发的伪造包（RichPresence 的 766、Cloud 的 147 响应）；
    //   2. 拆包 → RecvJob 分发；
    //   3. 处理函数要求替换时，把新的 header / body 写回这个包；
    //   4. 最后交给原函数 oRecvPkt，让 Steam 继续处理。
    HOOK_FUNC(RecvPkt, void*, void* pThis, CNetPacket* pPacket)
    {
        Hooks_NetPacket_RichPresence::TryInject(
            pThis, pPacket,
            [](void* pT, CNetPacket* pP) -> bool { return oRecvPkt(pT, pP) != nullptr; });

        Hooks_NetPacket_Cloud::Drain(
            pThis, pPacket,
            [](void* pT, CNetPacket* pP) -> bool { return oRecvPkt(pT, pP) != nullptr; });

        EMsg eMsg;
        const uint8 *pBody, *pHdr;
        uint32 cbBody, cbHdr;
        if (UnpackRaw(pPacket->m_pubData, pPacket->m_cubData,
                     eMsg, pHdr, cbHdr, pBody, cbBody)) {
            g_ResizedInPlace = false;
            RecvJob(eMsg, pBody, cbBody, pHdr, cbHdr);

            if (g_ResizedInPlace && g_NeedReplaceHdr) {
                // Body shrunk in-place + header changed -> full replace via pool
                ReplaceRecvPacket(pPacket,
                    g_NewHdr, g_cbNewHdr,
                    pBody, g_NewBodySize);
            } else if (g_ResizedInPlace) {
                pPacket->m_cubData = sizeof(MsgHdr) + cbHdr + g_NewBodySize;
            } else if (g_NeedReplaceHdr || g_NeedReplaceBody) {
                ReplaceRecvPacket(pPacket,
                    g_NeedReplaceHdr  ? g_NewHdr  : pHdr,
                    g_NeedReplaceHdr  ? g_cbNewHdr : cbHdr,
                    g_NeedReplaceBody ? g_NewBody : pBody,
                    g_NeedReplaceBody ? g_cbNewBody : cbBody);
            }
        }

        return oRecvPkt(pThis, pPacket);
    }

} // anonymous namespace


// 安装 / 卸载上面两个 hook（Detours），由 DLL 初始化和退出时调用。
namespace Hooks_NetPacket {
    void Install() {
        RESOLVE_C(PchMsgNameFromEMsg);
        HOOK_BEGIN();
        INSTALL_HOOK_C(BBuildAndAsyncSendFrame);
        INSTALL_HOOK_C(RecvPkt);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BBuildAndAsyncSendFrame);
        UNINSTALL_HOOK(RecvPkt);
        UNHOOK_END();
    }
}

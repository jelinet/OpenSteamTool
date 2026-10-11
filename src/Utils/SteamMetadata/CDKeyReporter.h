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

        // ── Steam 763 响应的原样内容（不做任何加工）──
        std::string rawHeader;          // 原始 protobuf header 字节（CMsgProtoBufHeader）
        std::string rawBody;            // 原始 protobuf body 字节（CMsgClientPurchaseResponse）
        int32_t     headerEresult = 0;  // header 里的 eresult
        std::string headerErrorMessage; // header 里的 error_message（Steam 有时会带错误说明）
        std::string receiptInfo;        // body 里的 purchase_receipt_info（二进制购买收据）
    };

    // 把结果归类成一个简短的状态字符串，例如 "activated"、"used_by_other_account"。
    const char* Status(const Report& r);

    // 你的上报接口返回的结果。接口按 Steam 763 响应的字段返回 JSON，例如：
    //   成功：{"eresult":1,"purchase_result_details":0,"error_message":""}
    //   失败：{"eresult":2,"purchase_result_details":15,"error_message":"key 已被使用"}
    // 字段含义和 Steam 的一样：eresult == 1 表示成功，purchase_result_details 是失败原因。
    // 只用来记录 / 写日志，不会写回 Steam 的包。
    struct ServerResponse {
        bool        httpOk     = false;  // HTTP 请求成功且状态码是 2xx
        uint32_t    httpStatus = 0;      // HTTP 状态码（请求没发出去时为 0）
        bool        parsed     = false;  // 返回的 body 里解析到了 eresult
        int32_t     eresult    = 0;      // 接口返回的 eresult（1 = 成功）
        int32_t     detail     = 0;      // 接口返回的 purchase_result_details
        std::string errorMessage;        // 接口返回的 error_message（没有时为空）
        std::string rawBody;             // 接口返回的原始 body，方便排查
    };

    // 同步发送：会阻塞调用方（Steam 网络线程）直到请求完成或超时。
    // 返回值见 ServerResponse；失败只写日志，不重试。
    ServerResponse SendReport(const Report& r);

    // ── 激活前的机器码校验 ──
    // （2026-10-11 从 {"allow":...} 改成 Steam 响应的字段：激活请求一律拦下，
    //  把接口返回的结果原样造成 Store.RegisterCDKey#1 的响应给界面。）
    // 你的校验接口返回 JSON，字段和 Steam 响应一致，例如：
    //   成功：{"eresult":1,"purchase_result_details":0,"error_message":""}
    //   失败：{"eresult":2,"purchase_result_details":15,"error_message":"key 已被使用"}
    // eresult 对应响应 header 的 eresult（1 = 成功，失败用 2），
    // purchase_result_details 对应响应 body（0 = 无错误，14 = 无效码，15 = 已被使用），
    // error_message 对应响应 header 的 error_message（可选）。
    struct CheckResponse {
        bool        httpOk     = false;  // HTTP 请求成功且状态码是 2xx
        uint32_t    httpStatus = 0;      // HTTP 状态码（请求没发出去时为 0）
        int32_t     eresult    = 2;      // 回给 Steam 界面的 eresult；默认 2（Fail）
        int32_t     detail     = 14;     // 回给 Steam 界面的 purchase_result_details；默认 14（无效 key）
        std::string errorMessage;        // 接口返回的 error_message（写进响应 header）
        std::string rawBody;             // 接口返回的原始 body，方便排查
    };

    // 激活前同步调用校验接口（在 Steam 网络线程里、激活请求发出之前执行）。
    // 请求 JSON：{"key":"...","steamid":"7656...","machine_id":"..."}
    // 接口连不上、超时、非 2xx、返回里没有 eresult 字段：eresult / detail 保持默认
    // （2 / 14），即按失败处理。
    CheckResponse CheckBeforeActivate(const std::string& key, uint64_t steamId);

    // 本机机器码：读注册表 HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
    // （Windows 安装时生成，重装系统会变）。读不到时返回空字符串。
    std::string GetMachineId();

    // 日志用：只显示 key 的最后 5 位。
    std::string MaskKey(const std::string& key);

} // namespace CDKeyReporter

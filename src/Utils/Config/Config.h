#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Config {

    enum class LogLevel { Trace, Debug, Info, Warn, Error };

    struct ManifestTimeouts {
        uint32_t resolve = 5000;
        uint32_t connect = 5000;
        uint32_t send    = 10000;
        uint32_t recv    = 10000;
    };

    struct InjectionSettings {
        bool enabled = false;
        std::string libraryX86;
        std::string libraryX64;
    };

    struct CloudSettings {
        bool enabled = false;
        std::string library;
    };

    // [cdkey] — CDKeyReporter 的两个接口。
    // 所有字段都不设默认地址：没配 report_url 就不上报，没配 check_url 就跳过校验并放行
    // （2026-10-13 和使用者确认；不把内网地址编进二进制）。
    struct CdkeySettings {
        std::string reportUrl;
        std::string checkUrl;
        std::string authToken;          // 非空时两个接口都带 "Authorization: Bearer <token>"
        uint32_t    timeoutMs = 2000;   // resolve/connect/send/recv 各用这个值
    };

    struct LoadResult {
        bool applied = false;
        bool luaPathsChanged = false;
    };

    LoadResult Load(const std::string& configPath);

    ManifestTimeouts GetManifestTimeouts();
    LogLevel GetLogLevel();
    std::string GetLogDir();
    std::vector<std::string> GetLuaPaths();
    std::string GetRemoteUrlTemplate();
    InjectionSettings GetInjectionSettings();
    CloudSettings GetCloudSettings();
    CdkeySettings GetCdkeySettings();
    bool GetStatsEnableApi();

    // [manifest] — provider selection lives in ManifestClient (table-driven).
    inline uint32_t manifestTimeoutResolve = 5000;
    inline uint32_t manifestTimeoutConnect = 5000;
    inline uint32_t manifestTimeoutSend    = 10000;
    inline uint32_t manifestTimeoutRecv    = 10000;

    // [log]
    inline LogLevel logLevel = LogLevel::Debug;

    // derived from configPath: <steam>/opensteamtool/
    inline std::string logDir;

    // [lua]
    inline std::vector<std::string> luaPaths;

    // [remote]
    inline std::string remoteUrlTemplate;

    // [stats]
    inline bool statsEnableApi = true;

    // [inject] - optional library injection into game processes.
    inline bool injectEnabled = false;
    inline std::string injectLibraryX86;
    inline std::string injectLibraryX64;

    // [cloud] - optional Steam Cloud save redirection via CloudRedirect.
    inline bool cloudEnabled = false;
    inline std::string cloudLibrary;

    // [cdkey]
    inline std::string cdkeyReportUrl;
    inline std::string cdkeyCheckUrl;
    inline std::string cdkeyAuthToken;
    inline uint32_t    cdkeyTimeoutMs = 2000;

}

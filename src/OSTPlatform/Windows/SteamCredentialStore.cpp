#include "include/SteamCredentialStore.h"

#include "include/Log.h"
#include "include/Numbers.h"
#include "include/Encoding.h"
#include "Windows/Handles.h"

#include <windows.h>

#include <utility>

namespace OSTPlatform::SteamCredentialStore {
namespace {

constexpr const wchar_t* kValueSteamId = L"SteamID";
constexpr const wchar_t* kValueAppTicket = L"AppTicket";
constexpr const wchar_t* kValueEncryptedTicket = L"ETicket";
constexpr const wchar_t* kActiveProcessKeyPath = L"Software\\Valve\\Steam\\ActiveProcess";
constexpr const wchar_t* kValueActiveUser = L"ActiveUser";
constexpr const wchar_t* kValueUniverse = L"Universe";

std::wstring SteamAppKeyPath(uint32_t appId) {
    return L"Software\\Valve\\Steam\\Apps\\" + std::to_wstring(appId);
}

const char* ValueNameForLog(PCWSTR valueName) {
    if (valueName == kValueSteamId) return "SteamID";
    if (valueName == kValueAppTicket) return "AppTicket";
    if (valueName == kValueEncryptedTicket) return "ETicket";
    if (valueName == kValueActiveUser) return "ActiveUser";
    if (valueName == kValueUniverse) return "Universe";
    return "?";
}

Status FromLStatus(LSTATUS status) {
    switch (status) {
    case ERROR_SUCCESS:
        return Status::Ok;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_NOT_FOUND:
        return Status::NotFound;
    default:
        return Status::Failed;
    }
}

// RegGetValueW does the size-dance for us: first call (pvData == nullptr)
// reports the byte count, second fills the buffer. `out` is overwritten only on
// success; left untouched on any failure.
LSTATUS ReadBinaryValue(uint32_t appId, PCWSTR valueName, std::vector<uint8_t>& out) {
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: reading {} for appid={}", ValueNameForLog(valueName), appId);
    const std::wstring keyPath = SteamAppKeyPath(appId);
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: reading {} for appid={} keyPath={}", ValueNameForLog(valueName), appId, keyPath);
    DWORD bytes = 0;
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, keyPath.c_str(), valueName,
                                  RRF_RT_REG_BINARY, nullptr, nullptr, &bytes);
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: read {} for appid={} status={} bytes={}", ValueNameForLog(valueName), appId, status, bytes);                              
    if (status != ERROR_SUCCESS) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to query {} size for appid={} status={}", ValueNameForLog(valueName), appId, status);
        const auto mapped = FromLStatus(status);
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: mapped status for {} appid={} is {}", ValueNameForLog(valueName), appId, ToString(mapped));
        if (mapped == Status::NotFound) {
            LOG_REGISTERCDKEY_INFO("SteamCredentialStore: {} missing for appid={} status={}",
                           ValueNameForLog(valueName), appId, status);
        } else {
            LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to query {} size for appid={} status={}",
                          ValueNameForLog(valueName), appId, status);
        }
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: returning mapped status for {} appid={} is {}", ValueNameForLog(valueName), appId, ToString(mapped));
        return status;
    }
    if (bytes == 0) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: {} empty for appid={}", ValueNameForLog(valueName), appId);
        out.clear();
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: returning ERROR_SUCCESS for {} appid={} (empty)", ValueNameForLog(valueName), appId);
        return ERROR_SUCCESS;
    }

    std::vector<uint8_t> buffer(bytes);
    status = RegGetValueW(HKEY_CURRENT_USER, keyPath.c_str(), valueName,
                          RRF_RT_REG_BINARY, nullptr, buffer.data(), &bytes);
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: read {} for appid={} status={} bytes={}", ValueNameForLog(valueName), appId, status, bytes);                      
    if (status != ERROR_SUCCESS) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to read {} for appid={} status={}",
                      ValueNameForLog(valueName), appId, status);
        return status;
    }

    buffer.resize(bytes);
    out = std::move(buffer);
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: read {} for appid={} bytes={}",
                   ValueNameForLog(valueName), appId, out.size());
    return ERROR_SUCCESS;
}

// Create-or-open the per-app key and set a single value. REG_OPTION_NON_VOLATILE
// + KEY_SET_VALUE mirrors what Steam itself uses for these credential entries.
LSTATUS WriteValue(uint32_t appId, PCWSTR valueName, DWORD type, const BYTE* data, DWORD bytes) {
    Windows::UniqueRegKey key;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, SteamAppKeyPath(appId).c_str(), 0, nullptr,
                                     REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, key.put(), nullptr);
    if (status != ERROR_SUCCESS) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to create/open app key appid={} status={}", appId, status);
        return status;
    }

    status = RegSetValueExW(key.get(), valueName, 0, type, data, bytes);
    if (status != ERROR_SUCCESS) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to write {} for appid={} bytes={} status={}",
                      ValueNameForLog(valueName), appId, bytes, status);
        return status;
    }

    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: wrote {} for appid={} bytes={}",
                  ValueNameForLog(valueName), appId, bytes);
    return ERROR_SUCCESS;
}

} // namespace

const char* ToString(Status status) {
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::NotFound: return "NotFound";
    case Status::Unsupported: return "Unsupported";
    case Status::Failed: return "Failed";
    }
    return "Unknown";
}

Status GetAppTicket(uint32_t appId, std::vector<uint8_t>& ticket) {
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: GetAppTicket for appid={}", appId);
    return FromLStatus(ReadBinaryValue(appId, kValueAppTicket, ticket));
}

Status WriteAppTicket(uint32_t appId, const std::vector<uint8_t>& data) {
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: WriteAppTicket for appid={}", appId);
    return FromLStatus(WriteValue(appId, kValueAppTicket, REG_BINARY,
                                  reinterpret_cast<const BYTE*>(data.data()), static_cast<DWORD>(data.size())));
}

Status GetETicket(uint32_t appId, std::vector<uint8_t>& ticket) {
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: GetETicket for appid={}", appId);
    return FromLStatus(ReadBinaryValue(appId, kValueEncryptedTicket, ticket));
}

Status WriteETicket(uint32_t appId, const std::vector<uint8_t>& data) {
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: WriteETicket for appid={}", appId);
    return FromLStatus(WriteValue(appId, kValueEncryptedTicket, REG_BINARY,
                                  reinterpret_cast<const BYTE*>(data.data()), static_cast<DWORD>(data.size())));
}

Status GetSteamId(uint32_t appId, uint64_t& steamId) {
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: GetSteamId for appid={}", appId);
    const std::wstring keyPath = SteamAppKeyPath(appId);
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: GetSteamId for appid={} keyPath={}", appId, keyPath);

    DWORD bytes = 0;
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, keyPath.c_str(), kValueSteamId,
                                  RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: queried SteamID size for appid={} status={} bytes={}", appId, status, bytes);                              
    if (status != ERROR_SUCCESS) {
        const auto mapped = FromLStatus(status);
        if (mapped == Status::NotFound) {
            LOG_REGISTERCDKEY_INFO("SteamCredentialStore: SteamID missing for appid={} status={}", appId, status);
        } else {
            LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to query SteamID size for appid={} status={}", appId, status);
        }
        return mapped;
    }
    if (bytes < sizeof(wchar_t)) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: SteamID value too short for appid={} bytes={}", appId, bytes);
        return Status::NotFound;
    }

    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, keyPath.c_str(), kValueSteamId,
                          RRF_RT_REG_SZ, nullptr, raw.data(), &bytes);
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: read SteamID for appid={} status={} bytes={}", appId, status, bytes);                      
    if (status != ERROR_SUCCESS) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to read SteamID for appid={} status={}", appId, status);
        return FromLStatus(status);
    }

    // RegGetValueW guarantees a single null terminator and counts it in `bytes`;
    // drop it so the parser sees only digits.
    const size_t chars = bytes / sizeof(wchar_t);
    raw.resize(chars > 0 ? chars - 1 : 0);

    // A present-but-empty/zero/unparseable value is treated as "no credential".
    const auto parsed = Numbers::ParseUInt64(raw);
    if (!parsed || *parsed == 0) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: SteamID value invalid for appid={}", appId);
        return Status::NotFound;
    }

    steamId = *parsed;
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: read SteamID for appid={} steamid={}", appId, steamId);
    return Status::Ok;
}

Status WriteSteamId(uint32_t appId, uint64_t steamId) {
    const std::wstring value = std::to_wstring(steamId);
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return FromLStatus(WriteValue(appId, kValueSteamId, REG_SZ,
                                  reinterpret_cast<const BYTE*>(value.c_str()), bytes));
}

Status GetActiveUser(uint32_t& accountId, std::wstring& universe) {
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: GetActiveUser");
    DWORD activeUser = 0;
    DWORD bytes = sizeof(activeUser);
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kActiveProcessKeyPath, kValueActiveUser,
                                  RRF_RT_REG_DWORD, nullptr, &activeUser, &bytes);

    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: queried ActiveUser size status={} bytes={}", status, bytes);
    if (status != ERROR_SUCCESS) {
        const auto mapped = FromLStatus(status);
        if (mapped == Status::NotFound) {
            LOG_REGISTERCDKEY_INFO("SteamCredentialStore: ActiveUser missing status={}", status);
        } else {
            LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to read ActiveUser status={}", status);
        }
        return mapped;
    }
    if (bytes != sizeof(activeUser)) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: ActiveUser has unexpected size bytes={}", bytes);
        return Status::NotFound;
    }
    if (activeUser == 0) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: ActiveUser is zero");
        return Status::NotFound;
    }

    bytes = 0;
    status = RegGetValueW(HKEY_CURRENT_USER, kActiveProcessKeyPath, kValueUniverse,
                          RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: queried ActiveProcess Universe size status={} bytes={}", status, bytes);
    if (status != ERROR_SUCCESS) {
        const auto mapped = FromLStatus(status);
        if (mapped == Status::NotFound) {
            LOG_REGISTERCDKEY_INFO("SteamCredentialStore: ActiveProcess Universe missing status={}", status);
        } else {
            LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to query ActiveProcess Universe size status={}", status);
        }
        return mapped;
    }
    if (bytes < sizeof(wchar_t)) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: ActiveProcess Universe value too short bytes={}", bytes);
        return Status::NotFound;
    }

    std::wstring rawUniverse(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, kActiveProcessKeyPath, kValueUniverse,
                          RRF_RT_REG_SZ, nullptr, rawUniverse.data(), &bytes);
    if (status != ERROR_SUCCESS) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: failed to read ActiveProcess Universe status={}", status);
        return FromLStatus(status);
    }

    const size_t chars = bytes / sizeof(wchar_t);
    rawUniverse.resize(chars > 0 ? chars - 1 : 0);
    if (rawUniverse.empty()) {
        LOG_REGISTERCDKEY_INFO("SteamCredentialStore: ActiveProcess Universe is empty");
        return Status::NotFound;
    }

    accountId = activeUser;
    universe = std::move(rawUniverse);
    LOG_REGISTERCDKEY_INFO("SteamCredentialStore: read ActiveUser accountid={} universe={}({} chars)",
                           accountId, Encoding::WideToUtf8(universe), universe.size());
    return Status::Ok;
}

} // namespace OSTPlatform::SteamCredentialStore

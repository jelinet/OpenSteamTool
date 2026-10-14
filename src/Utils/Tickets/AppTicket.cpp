#include "AppTicket.h"
#include "Hook/Hooks_Decryption.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"

namespace AppTicket {
    constexpr AppId_t kLocalAppTicketSourceAppId = 7;
    constexpr size_t kSteamIdTicketMinimumSize = 16;

    static uint64_t GetSteamIDFromCredentialStore(AppId_t appId) {
        uint64_t steamId = 0;
        const auto status = OSTPlatform::SteamCredentialStore::GetSteamId(appId, steamId);
        LOG_REGISTERCDKEY_DEBUG("GetSpoofSteamID for AppId {}: credential store status = {}", appId, OSTPlatform::SteamCredentialStore::ToString(status));
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_REGISTERCDKEY_DEBUG("GetSpoofSteamID for AppId {}: SteamID unavailable in credential store ({}),ruturn 0",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return 0;
        }

        LOG_REGISTERCDKEY_DEBUG("GetSpoofSteamID for AppId {}: SteamID credential -> 0x{:X}({})", appId, steamId, steamId);
        return steamId;
    }

    std::vector<uint8_t> GetAppOwnershipTicketFromCredentialStore(AppId_t appId) {
        // exclude those appids that are not in addappid
        LOG_REGISTERCDKEY_DEBUG("GetAppOwnershipTicketFromCredentialStore for AppId {}: checking credential store", appId);
        if (!LuaConfig::HasDepot(appId)) {
            LOG_REGISTERCDKEY_DEBUG("GetAppOwnershipTicketFromCredentialStore for AppId {}: not in addappid, skip", appId);
            return {};
        }
        std::vector<uint8_t> ticket;
        const auto status = OSTPlatform::SteamCredentialStore::GetAppTicket(appId, ticket);
        LOG_REGISTERCDKEY_DEBUG("Read App Ownership Ticket for AppId {}: credential store status = {}", appId, OSTPlatform::SteamCredentialStore::ToString(status));
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_REGISTERCDKEY_DEBUG("Read App Ownership Ticket for AppId {}: cached credential unavailable ({})",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return {};
        }

        LOG_INFO("Successfully retrieved App Ownership Ticket from credential store, AppId: {}, Ticket Size: {}", appId, ticket.size());
        return ticket;
    }

    // Exploit steamdrmp's off-by-four ticket parsing vulnerability:
    static std::vector<uint8_t> ForgeLocalAppOwnershipTicket(AppId_t appId) {
        std::vector<uint8_t> source = Hooks_Decryption::GetCacheAppOwnershipTicket(kLocalAppTicketSourceAppId);
        LOG_REGISTERCDKEY_DEBUG("ForgeLocalAppOwnershipTicket for AppId {}: source ticket size = {}", appId, source.size());
        if (source.size() <= kAppTicketSignatureSize) {
            LOG_REGISTERCDKEY_DEBUG("ForgeLocalAppOwnershipTicket for AppId {}: no source appticket", appId);
            return {};
        }

        const size_t signedSize = source.size() - kAppTicketSignatureSize;
        std::vector<uint8_t> ticket;
        ticket.reserve(source.size() + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin(), source.begin() + signedSize);

        const uint8_t* appIdBytes = reinterpret_cast<const uint8_t*>(&appId);
        ticket.insert(ticket.end(), appIdBytes, appIdBytes + sizeof(AppId_t));
        ticket.insert(ticket.end(), source.begin() + signedSize, source.end());

        LOG_REGISTERCDKEY_DEBUG("Forged App Ownership Ticket, AppId: {}, SourceAppId: {}, Physical Size: {}, Total Size: {}",
                 appId, kLocalAppTicketSourceAppId, ticket.size(), source.size());
        return ticket;
    }

    bool GetAppOwnershipTicket(AppId_t appId, AppOwnershipTicket& ticket, AppTicketSource source) {
        ticket = {};
        LOG_REGISTERCDKEY_DEBUG("GetAppOwnershipTicket for AppId {}: source = {}", appId, static_cast<int>(source));
        if (source == AppTicketSource::CredentialStoreOnly || source == AppTicketSource::CredentialStoreThenForge) {
            ticket.data = GetAppOwnershipTicketFromCredentialStore(appId);
            if (!ticket.data.empty() && ticket.data.size() >= sizeof(uint32)) {
                ticket.totalSize = static_cast<uint32>(ticket.data.size());
                ticket.appIdOffset = kAppTicketAppIdOffset;
                ticket.steamIdOffset = kAppTicketSteamIdOffset;
                ticket.signatureOffset = *reinterpret_cast<const uint32*>(ticket.data.data());
                ticket.signatureSize = kAppTicketSignatureSize;
                LOG_REGISTERCDKEY_DEBUG("GetAppOwnershipTicket for AppId {}: retrieved from credential store, totalSize={}, signatureOffset={}",
                         appId, ticket.totalSize, ticket.signatureOffset);
                return true;
            }
        }

        if (source == AppTicketSource::CredentialStoreOnly) 
        {
            LOG_REGISTERCDKEY_DEBUG("GetAppOwnershipTicket for AppId {}: only credential store allowed, but no ticket found", appId);
            return false;
        }

        ticket.data = ForgeLocalAppOwnershipTicket(appId);
        if (ticket.data.empty()) 
        {
            LOG_REGISTERCDKEY_DEBUG("GetAppOwnershipTicket for AppId {}: failed to forge local ticket", appId);
            return false;
        }

        ticket.totalSize = static_cast<uint32>(ticket.data.size() - sizeof(AppId_t));
        ticket.appIdOffset = ticket.totalSize - kAppTicketSignatureSize;
        ticket.steamIdOffset = kAppTicketSteamIdOffset;
        ticket.signatureOffset = ticket.appIdOffset + sizeof(AppId_t);
        ticket.signatureSize = kAppTicketSignatureSize;
        LOG_REGISTERCDKEY_DEBUG("GetAppOwnershipTicket for AppId {}: forged local ticket, totalSize={}, signatureOffset={}",
                 appId, ticket.totalSize, ticket.signatureOffset);
        return true;
    }

    std::vector<uint8_t> GetEncryptedTicketFromCredentialStore(AppId_t appId) {
        LOG_REGISTERCDKEY_DEBUG("appid={}", appId);    
        // exclude those appids that are not in addappid
        if (!LuaConfig::HasDepot(appId)) {
            LOG_REGISTERCDKEY_DEBUG("GetEncryptedTicketFromCredentialStore for AppId {}: not in addappid, skip", appId);
            return {};
        }
        std::vector<uint8_t> ticket;
        const auto status = OSTPlatform::SteamCredentialStore::GetETicket(appId, ticket);
        LOG_REGISTERCDKEY_DEBUG("Read Encrypted App Ticket for AppId {}: credential store status = {}", appId, OSTPlatform::SteamCredentialStore::ToString(status));
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_REGISTERCDKEY_DEBUG("Read Encrypted App Ticket for AppId {}: cached credential unavailable ({})",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return {};
        }

        LOG_INFO("Successfully retrieved Encrypted App Ticket from credential store, AppId: {}, Ticket Size: {}", appId, ticket.size());
        return ticket;
    }

    bool WriteAppOwnershipTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        // we can't execlude appids here 
        LOG_REGISTERCDKEY_DEBUG("WriteAppOwnershipTicket for AppId {}: writing to credential store, data size = {}", appId, data.size());
        const auto status = OSTPlatform::SteamCredentialStore::WriteAppTicket(appId, data);
        LOG_REGISTERCDKEY_DEBUG("WriteAppOwnershipTicket for AppId {}: credential store status = {}", appId, OSTPlatform::SteamCredentialStore::ToString(status));
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_REGISTERCDKEY_DEBUG("Failed to write AppTicket for AppId {} to credential store: {}",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return false;
        }

        LOG_REGISTERCDKEY_DEBUG("Wrote AppTicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteEncryptedTicket(AppId_t appId, const std::vector<uint8_t>& data) {
        // we can't execlude appids here 
        LOG_REGISTERCDKEY_DEBUG("WriteEncryptedTicket for AppId {}: writing to credential store, data size = {}", appId, data.size());
        const auto status = OSTPlatform::SteamCredentialStore::WriteETicket(appId, data);
        LOG_REGISTERCDKEY_DEBUG("WriteEncryptedTicket for AppId {}: credential store status = {}", appId, OSTPlatform::SteamCredentialStore::ToString(status));
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_REGISTERCDKEY_DEBUG("Failed to write ETicket for AppId {} to credential store: {}",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return false;
        }

        LOG_INFO("Wrote ETicket for AppId {} ({} bytes)", appId, data.size());
        return true;
    }

    bool WriteSteamID(AppId_t appId, uint64_t steamId) {
        LOG_REGISTERCDKEY_DEBUG("WriteSteamID for AppId {}: writing to credential store, steamId = 0x{:X}({})", appId, steamId, steamId);
        const auto status = OSTPlatform::SteamCredentialStore::WriteSteamId(appId, steamId);
        LOG_REGISTERCDKEY_DEBUG("WriteSteamID for AppId {}: credential store status = {}", appId, OSTPlatform::SteamCredentialStore::ToString(status));
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_REGISTERCDKEY_DEBUG("Failed to write SteamID for AppId {} to credential store: {}",
                      appId, OSTPlatform::SteamCredentialStore::ToString(status));
            return false;
        }

        LOG_INFO("Wrote SteamID for AppId {} ({})", appId, steamId);
        return true;
    }

    uint64_t GetSpoofSteamID(AppId_t appId) {
        // exclude those appids that are not in addappid
        LOG_REGISTERCDKEY_DEBUG("GetSpoofSteamID for AppId {}: checking credential store", appId);
        if (!LuaConfig::HasDepot(appId)) {
            LOG_REGISTERCDKEY_DEBUG("GetSpoofSteamID for AppId {}: not in addappid, skip spoofing", appId);
            return 0;
        }
        const uint64_t credentialSteamID = GetSteamIDFromCredentialStore(appId);
        LOG_REGISTERCDKEY_DEBUG("GetSpoofSteamID for AppId {}: credential store SteamID = 0x{:X}({})", appId, credentialSteamID, credentialSteamID);
        if (credentialSteamID != 0) {
            LOG_REGISTERCDKEY_DEBUG("GetSpoofSteamID for AppId {}: using credential store SteamID = 0x{:X}({})", appId, credentialSteamID, credentialSteamID);
            return credentialSteamID;
        }

        // The SteamID baked into the cached AppOwnershipTicket is the same
        // one Steam itself uses for this app — pull it straight out of the
        // ticket so spoofed responses match what the DRM layer expects.
        // Layout: ticket bytes start with [uint32 Size][uint32 Version][uint64 SteamID][...].
        std::vector<uint8_t> ticket = GetAppOwnershipTicketFromCredentialStore(appId);
        LOG_REGISTERCDKEY_DEBUG("GetSpoofSteamID for AppId {}: credential store ticket size = {}", appId, ticket.size());
        if (ticket.size() >= kSteamIdTicketMinimumSize) {
            const uint64_t steamID = reinterpret_cast<const uint64_t*>(ticket.data())[1];
            LOG_REGISTERCDKEY_DEBUG("GetSpoofSteamID for AppId {}: -> 0x{:X}({})", appId, steamID, steamID);
            return steamID;
        }
        return 0;
    }
}

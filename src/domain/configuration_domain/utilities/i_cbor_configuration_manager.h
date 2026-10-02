#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <memory_resource>
#include <vector>

#include "configuration/services/stored_cbor_info.h"

namespace eerie_leap::domain::configuration_domain::utilities {

using eerie_leap::configuration::services::StoredCborInfo;

class ICborConfigurationManager {
public:
    virtual ~ICborConfigurationManager() = default;

    /**
     * @brief Validates the CBOR and stores it as it is.
     * @param reason Receives why it failed, NUL-terminated and truncated to fit; may be empty.
     */
    virtual bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) = 0;
    /** @return The stored CBOR, empty without a configuration. */
    virtual std::pmr::vector<uint8_t> GetCborConfiguration() = 0;
    /** @return Size and CRC32 of the stored CBOR, zero without a configuration. */
    virtual StoredCborInfo GetCborConfigurationInfo() = 0;

    // Non-virtual shorthand: default arguments on virtuals bind to the static type.
    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) {
        return ApplyCborConfiguration(cbor_data, {});
    }

protected:
    /** @brief Copies @p message into @p reason, truncated to fit and NUL-terminated. */
    static void SetReason(std::span<char> reason, std::string_view message) {
        if(reason.empty())
            return;

        size_t length = std::min(message.size(), reason.size() - 1);
        // Never splits a UTF-8 character.
        while(length > 0 && length < message.size() && (static_cast<uint8_t>(message[length]) & 0xC0) == 0x80)
            length--;

        std::copy_n(message.data(), length, reason.data());
        reason[length] = '\0';
    }
};

} // namespace eerie_leap::domain::configuration_domain::utilities

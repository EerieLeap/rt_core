#pragma once

#include <cstdint>
#include <span>
#include <memory_resource>
#include <vector>

#include "configuration/services/stored_cbor_info.h"

namespace eerie_leap::domain::configuration_domain::utilities {

using eerie_leap::configuration::services::StoredCborInfo;

class ICborConfigurationManager {
public:
    virtual ~ICborConfigurationManager() = default;

    /** @brief Validates the CBOR and stores it as it is. */
    virtual bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) = 0;
    /** @return The stored CBOR, empty without a configuration. */
    virtual std::pmr::vector<uint8_t> GetCborConfiguration() = 0;
    /** @return Size and CRC32 of the stored CBOR, zero without a configuration. */
    virtual StoredCborInfo GetCborConfigurationInfo() = 0;
};

} // namespace eerie_leap::domain::configuration_domain::utilities

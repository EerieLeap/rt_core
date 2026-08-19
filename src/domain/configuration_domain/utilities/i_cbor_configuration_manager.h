#pragma once

#include <cstdint>
#include <span>
#include <memory_resource>
#include <vector>

namespace eerie_leap::domain::configuration_domain::utilities {

class ICborConfigurationManager {
public:
    virtual ~ICborConfigurationManager() = default;

    virtual bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) = 0;
    virtual std::pmr::vector<uint8_t> GetCborConfiguration() = 0;
};

} // namespace eerie_leap::domain::configuration_domain::utilities

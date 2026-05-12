#pragma once

#include <eerie_memory.hpp>

#include "configuration/cbor/cbor_system_config/cbor_system_config.h"
#include "domain/system_domain/models/system_configuration.h"

namespace eerie_leap::domain::system_domain::configuration::parsers {

using eerie_leap::domain::system_domain::models::SystemConfiguration;

class SystemConfigurationCborParser {
public:
    SystemConfigurationCborParser() = default;

    eerie_memory::pmr_unique_ptr<CborSystemConfig> Serialize(const SystemConfiguration& configuration);
    eerie_memory::pmr_unique_ptr<SystemConfiguration> Deserialize(std::pmr::memory_resource* mr, const CborSystemConfig& config);
};

} // namespace eerie_leap::domain::system_domain::configuration::parsers
